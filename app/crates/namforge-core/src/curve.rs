//! Pictures of an impulse response for the UI: its waveform as a column of minima and maxima,
//! and its frequency response on a logarithmic frequency axis.

/// The frequency axis of [`response`]: its points are spread evenly, on a logarithmic scale,
/// from `RESPONSE_MIN_HZ` to `RESPONSE_MAX_HZ`.
pub const RESPONSE_MIN_HZ: f64 = 20.0;
pub const RESPONSE_MAX_HZ: f64 = 20000.0;
pub const RESPONSE_POINTS: usize = 120;
/// The most columns [`wave`] returns.
pub const WAVE_COLUMNS: usize = 400;

/// The waveform as at most `WAVE_COLUMNS` columns of `[lowest, highest]` sample, scaled so
/// that the largest sample is 1 (or -1).
pub fn wave(samples: &[f32]) -> Vec<[f32; 2]> {
    let peak = samples.iter().fold(0.0f32, |p, s| p.max(s.abs()));
    if samples.is_empty() || peak <= 0.0 {
        return Vec::new();
    }
    let columns = samples.len().min(WAVE_COLUMNS);
    (0..columns)
        .map(|c| {
            let (from, to) = (c * samples.len() / columns, ((c + 1) * samples.len() / columns).max(c * samples.len() / columns + 1));
            let part = &samples[from..to.min(samples.len())];
            let lo = part.iter().fold(f32::MAX, |a, &s| a.min(s));
            let hi = part.iter().fold(f32::MIN, |a, &s| a.max(s));
            [lo / peak, hi / peak]
        })
        .collect()
}

/// The gain in dB at `RESPONSE_POINTS` frequencies, each the average power over a sixth of an
/// octave around it (a raw FFT is too ragged to read). Empty for an empty or silent IR.
pub fn response(samples: &[f32], sample_rate: f64) -> Vec<f32> {
    if samples.iter().all(|s| *s == 0.0) {
        return Vec::new();
    }
    let n = (samples.len() * 2).next_power_of_two().max(8192);
    let mut re: Vec<f64> = samples.iter().map(|s| *s as f64).collect();
    re.resize(n, 0.0);
    let mut im = vec![0.0f64; n];
    fft(&mut re, &mut im);
    let power: Vec<f64> = (0..=n / 2).map(|k| re[k] * re[k] + im[k] * im[k]).collect();
    let bin = sample_rate / n as f64;
    let half = 2f64.powf(1.0 / 12.0);
    (0..RESPONSE_POINTS)
        .map(|i| {
            let f = RESPONSE_MIN_HZ * (RESPONSE_MAX_HZ / RESPONSE_MIN_HZ).powf(i as f64 / (RESPONSE_POINTS - 1) as f64);
            let nearest = ((f / bin).round() as usize).clamp(1, n / 2);
            let from = ((f / half / bin).ceil() as usize).clamp(1, nearest);
            let to = ((f * half / bin).floor() as usize).clamp(nearest, n / 2);
            let mean = power[from..=to].iter().sum::<f64>() / (to - from + 1) as f64;
            (10.0 * mean.max(1e-20).log10()) as f32
        })
        .collect()
}

/// In-place radix-2 FFT; the length must be a power of two.
fn fft(re: &mut [f64], im: &mut [f64]) {
    let n = re.len();
    let mut j = 0;
    for i in 1..n {
        let mut bit = n >> 1;
        while j & bit != 0 {
            j ^= bit;
            bit >>= 1;
        }
        j |= bit;
        if i < j {
            re.swap(i, j);
            im.swap(i, j);
        }
    }
    let mut len = 2;
    while len <= n {
        let angle = -2.0 * std::f64::consts::PI / len as f64;
        let (wr, wi) = (angle.cos(), angle.sin());
        for start in (0..n).step_by(len) {
            let (mut cr, mut ci) = (1.0f64, 0.0f64);
            for k in 0..len / 2 {
                let (a, b) = (start + k, start + k + len / 2);
                let (tr, ti) = (re[b] * cr - im[b] * ci, re[b] * ci + im[b] * cr);
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                let next = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = next;
            }
        }
        len <<= 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn at(curve: &[f32], hz: f64) -> f32 {
        let i = ((hz / RESPONSE_MIN_HZ).ln() / (RESPONSE_MAX_HZ / RESPONSE_MIN_HZ).ln() * (RESPONSE_POINTS - 1) as f64).round() as usize;
        curve[i]
    }

    #[test]
    fn an_impulse_is_flat() {
        let mut ir = vec![0.0f32; 512];
        ir[3] = 0.5;
        let curve = response(&ir, 48000.0);
        assert_eq!(curve.len(), RESPONSE_POINTS);
        for db in curve {
            assert!((db + 6.02).abs() < 0.01, "{db}");
        }
    }

    #[test]
    fn a_one_pole_lowpass_is_3_db_down_at_its_corner() {
        // y[n] = a x[n] + (1 - a) y[n-1], corner at 1 kHz.
        let a = 1.0 - (-2.0 * std::f64::consts::PI * 1000.0 / 48000.0).exp();
        let ir: Vec<f32> = (0..4096).map(|n| (a * (1.0 - a).powi(n)) as f32).collect();
        let curve = response(&ir, 48000.0);
        assert!(at(&curve, 50.0).abs() < 0.1, "{}", at(&curve, 50.0));
        assert!((at(&curve, 1000.0) + 3.0).abs() < 0.3, "{}", at(&curve, 1000.0));
        assert!((at(&curve, 10000.0) + 20.0).abs() < 1.5, "{}", at(&curve, 10000.0));
    }

    #[test]
    fn the_wave_keeps_peaks_and_its_scale() {
        let mut ir = vec![0.0f32; 4000];
        ir[10] = -0.25;
        ir[2000] = 0.125;
        let w = wave(&ir);
        assert_eq!(w.len(), WAVE_COLUMNS);
        assert_eq!(w[1], [-1.0, 0.0]);
        assert_eq!(w[200], [0.0, 0.5]);
        assert_eq!(wave(&[0.5, -0.5]).len(), 2);
        assert!(wave(&[]).is_empty() && wave(&[0.0; 8]).is_empty() && response(&[0.0; 8], 48000.0).is_empty());
    }
}
