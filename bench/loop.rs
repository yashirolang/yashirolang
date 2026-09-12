use std::time::Instant;
const N: i64 = 20000000;
fn main() {
    let t0 = Instant::now();
    let mut s: i64 = 0;
    for i in 0..N { s = (s + i * 3 + 7) % 1000000007; }
    let ms = t0.elapsed().as_secs_f64() * 1000.0;
    println!("RESULT {}", s);
    println!("TIME_MS {:.3}", ms);
}
