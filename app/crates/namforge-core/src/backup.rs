//! Copies of the unit's SD card on the Mac: every amp model, IR, preset and drum song the unit
//! uses, in a folder laid out like the card, and the way back.

use std::io::{Read, Write};
use std::path::{Path, PathBuf};

use crate::session::{Session, SessionError};
use crate::types::Event;

/// The card's folders that hold what the unit uses. In the root only .wav files (IRs) count.
const FOLDERS: [&str; 5] = ["/AMP", "/IR", "/PRESETS", "/DRUMS", "/"];

fn job(emit: &impl Fn(Event), title: &str, detail: &str, done: u32, total: u32, finished: bool, error: &str) {
    emit(Event::Job { title: title.into(), detail: detail.into(), done, total, finished, error: error.into() });
}

fn is_wav(name: &str) -> bool {
    name.to_ascii_lowercase().ends_with(".wav")
}

/// Copies the card into `dest` (created if needed). A problem with the card or the Mac's disk
/// ends the job with a message; only a lost connection is returned as an error.
pub fn backup<T: Read + Write>(s: &mut Session<T>, dest: &Path, emit: &impl Fn(Event)) -> Result<(), SessionError> {
    const TITLE: &str = "Backup";
    let refused = |e: SessionError, what: &str| -> Result<String, SessionError> {
        match e {
            SessionError::Device { message, .. } => Ok(format!("{what}: {message}")),
            other => Err(other),
        }
    };
    job(emit, TITLE, "Reading the card's folders", 0, 0, false, "");
    let mut files: Vec<(String, u32)> = Vec::new();
    for folder in FOLDERS {
        let list = match s.dir_list(folder) {
            Ok(list) => list,
            Err(e) => {
                let why = refused(e, folder)?;
                job(emit, TITLE, "", 0, 0, true, &why);
                return Ok(());
            }
        };
        for (name, size, is_folder) in list {
            if is_folder || (folder == "/" && !is_wav(&name)) {
                continue;
            }
            files.push((if folder == "/" { format!("/{name}") } else { format!("{folder}/{name}") }, size));
        }
    }
    let total = files.len() as u32;
    let mut bytes = 0u64;
    for (i, (path, size)) in files.iter().enumerate() {
        job(emit, TITLE, path, i as u32, total, false, "");
        let data = match s.get_file(path, |_, _| {}) {
            Ok(data) => data,
            Err(e) => {
                let why = refused(e, path)?;
                job(emit, TITLE, "", i as u32, total, true, &why);
                return Ok(());
            }
        };
        if data.len() as u32 != *size {
            job(emit, TITLE, "", i as u32, total, true, &format!("{path} arrived with the wrong size"));
            return Ok(());
        }
        let local = dest.join(path.trim_start_matches('/'));
        let written = local.parent().map_or(Ok(()), std::fs::create_dir_all).and_then(|_| std::fs::write(&local, &data));
        if let Err(e) = written {
            job(emit, TITLE, "", i as u32, total, true, &format!("Cannot write {}: {e}", local.display()));
            return Ok(());
        }
        bytes += data.len() as u64;
    }
    let summary = format!("{total} files, {:.1} MB, in {}", bytes as f64 / 1_048_576.0, dest.display());
    job(emit, TITLE, &summary, total, total, true, "");
    Ok(())
}

/// The files of a backup folder with the card paths they go back to.
pub fn restorable(src: &Path) -> Vec<(PathBuf, String)> {
    let visible = |name: &str| !name.starts_with('.');
    let mut out = Vec::new();
    let entries = |dir: &Path| -> Vec<(PathBuf, String)> {
        let mut list: Vec<(PathBuf, String)> = std::fs::read_dir(dir)
            .into_iter()
            .flatten()
            .flatten()
            .filter_map(|e| Some((e.path(), e.file_name().into_string().ok()?)))
            .filter(|(_, name)| visible(name))
            .collect();
        list.sort();
        list
    };
    for (path, name) in entries(src) {
        if path.is_file() && is_wav(&name) {
            out.push((path, format!("/{name}")));
        } else if path.is_dir() && FOLDERS.contains(&format!("/{}", name.to_ascii_uppercase()).as_str()) {
            let folder = name.to_ascii_uppercase();
            for (file, file_name) in entries(&path) {
                if file.is_file() {
                    out.push((file, format!("/{folder}/{file_name}")));
                }
            }
        }
    }
    out
}

/// Sends a backup folder's files back to the card, replacing files of the same name. Files
/// on the card that the backup does not have are left alone.
pub fn restore<T: Read + Write>(s: &mut Session<T>, src: &Path, emit: &impl Fn(Event)) -> Result<(), SessionError> {
    const TITLE: &str = "Restore";
    let files = restorable(src);
    let total = files.len() as u32;
    if total == 0 {
        job(emit, TITLE, "", 0, 0, true, "This folder is not a backup: it has no AMP, IR, PRESETS or DRUMS folder with files.");
        return Ok(());
    }
    let mut problems: Vec<String> = Vec::new();
    for (i, (local, path)) in files.iter().enumerate() {
        job(emit, TITLE, path, i as u32, total, false, "");
        let sent = std::fs::read(local).map_err(|e| e.to_string()).and_then(|data| match s.put_file(path, &data, |_| {}) {
            Ok(()) => Ok(Ok(())),
            Err(SessionError::Device { message, .. }) => Err(message),
            Err(e) => Ok(Err(e)),
        });
        match sent {
            Ok(Ok(())) => {}
            Ok(Err(lost)) => return Err(lost),
            Err(why) => problems.push(format!("{path}: {why}")),
        }
    }
    let ok = total - problems.len() as u32;
    let summary = format!("{ok} of {total} files sent");
    job(emit, TITLE, &summary, total, total, true, &problems.join("; "));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_backup_folder_maps_back_to_card_paths() {
        let dir = std::env::temp_dir().join(format!("namforge-restorable-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        for (file, text) in [("AMP/a.nam", "x"), ("IR/b.wav", "x"), ("PRESETS/01 Lead.txt", "x"), ("root.wav", "x"), ("notes.txt", "x"), ("AMP/.hidden", "x"), ("Other/c.nam", "x")] {
            let p = dir.join(file);
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, text).unwrap();
        }
        let paths: Vec<String> = restorable(&dir).into_iter().map(|(_, card)| card).collect();
        assert_eq!(paths, ["/AMP/a.nam", "/IR/b.wav", "/PRESETS/01 Lead.txt", "/root.wav"]);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
