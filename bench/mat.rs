use std::time::Instant;
const N: usize = 512;

fn make(seed: i64) -> Vec<Vec<f64>> {
    let mut m: Vec<Vec<f64>> = Vec::new();
    let mut s = seed;
    for _i in 0..N {
        let mut row: Vec<f64> = Vec::new();
        for _j in 0..N {
            s = (s * 1103515 + 12345) % 2147483648;
            row.push((s % 1000) as f64 / 1000.0);
        }
        m.push(row);
    }
    m
}

fn main() {
    let a = make(1);
    let b = make(7);
    let mut c: Vec<Vec<f64>> = vec![vec![0.0; N]; N];

    let t0 = Instant::now();
    for i in 0..N {
        for k in 0..N {
            let aik = a[i][k];
            for j in 0..N { c[i][j] = c[i][j] + aik * b[k][j]; }
        }
    }
    let ms = t0.elapsed().as_secs_f64() * 1000.0;

    let mut t = 0.0;
    for i in 0..N { t += c[i][i]; }
    println!("RESULT {:.6}", t);
    println!("TIME_MS {:.3}", ms);
}
