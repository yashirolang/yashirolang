use std::time::Instant;
const N: i64 = 20000000;
fn main() {
    let t0 = Instant::now();
    let mut s: f64 = 0.0;
    let mut sign: f64 = 1.0;
    let mut i: i64 = 0;
    while i < N {
        s += sign / ((2 * i + 1) as f64);
        sign = -sign;
        i += 1;
    }
    let ms = t0.elapsed().as_secs_f64() * 1000.0;
    println!("RESULT {:.6}", s * 4.0);
    println!("TIME_MS {:.3}", ms);
}
