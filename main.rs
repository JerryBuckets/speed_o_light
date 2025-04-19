use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::thread;
use std::time::Duration;

const DEVICE_PATH: &str    = "/dev/project";
const SYS_CLASS_PATH: &str = "/sys/class/project/project";
const SYS_LED_FILES: [&str; 3] = ["led1_duty", "led2_duty", "led3_duty"];
const MAX_PRESS: u32        = 110;
const SLEEP_MS: u64         = 500;

/// Read the last‑10 s press count; panics on I/O error.
fn read_count() -> u32 {
    let mut buf = String::new();
    let mut f = File::open(DEVICE_PATH)
        .expect("cannot open /dev/project for reading");
    f.read_to_string(&mut buf)
        .expect("cannot read from /dev/project");
    buf.trim().parse::<u32>().unwrap_or(0)
}

/// Compute output percentage in the range 0–100.
fn compute_output_pct(count: u32) -> u32 {
    if count >= MAX_PRESS {
        100
    } else {
        count.saturating_mul(100) / MAX_PRESS
    }
}

/// Write percentage to the character device.
fn write_dev(pct: u32) {
    let mut f = OpenOptions::new()
        .write(true)
        .open(DEVICE_PATH)
        .expect("cannot open /dev/project for writing");
    let s = format!("{}\n", pct);
    f.write_all(s.as_bytes())
        .expect("cannot write to /dev/project");
}

/// Write percentage to the three sysfs LED duty files.
fn write_sys(pct: u32) {
    // split pct across three LEDs
    let scaled = pct.saturating_mul(3);
    let d1 = scaled.min(100);
    let d2 = (scaled.saturating_sub(100)).min(100);
    let d3 = (scaled.saturating_sub(200)).min(100);
    let duties = [d1, d2, d3];

    // Print one line summarizing all three
    println!(
        "WRITE sysfs -> {}={}, {}={}, {}={}\n",
        SYS_LED_FILES[0], d1,
        SYS_LED_FILES[1], d2,
        SYS_LED_FILES[2], d3,
    );

    for (i, file) in SYS_LED_FILES.iter().enumerate() {
        let path = format!("{}/{}", SYS_CLASS_PATH, file);
        fs::write(&path, format!("{}\n", duties[i]))
            .unwrap_or_else(|e| panic!("failed to write {} to {}: {}", duties[i], path, e));
    }
}

fn dev_loop() {
    println!("MODE --dev: writing to {}", DEVICE_PATH);
    loop {
        let count = read_count();
        println!("READ count = {}", count);

        let pct = compute_output_pct(count);
        println!("CALC output = {}%", pct);

        write_dev(pct);
        println!("WRITE {} -> {}\n", pct, DEVICE_PATH);

        thread::sleep(Duration::from_millis(SLEEP_MS));
    }
}

fn sys_loop() {
    println!("MODE --sys: writing to {}/<led?_duty>", SYS_CLASS_PATH);
    loop {
        let count = read_count();
        println!("READ count = {}", count);

        let pct = compute_output_pct(count);
        println!("CALC output = {}%", pct);

        write_sys(pct);

        thread::sleep(Duration::from_millis(SLEEP_MS));
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        eprintln!("Usage: {} [--dev | --sys]", args[0]);
        return;
    }
    match args[1].as_str() {
        "--dev" => dev_loop(),
        "--sys" => sys_loop(),
        arg     => eprintln!("Unrecognized argument '{}'. Use --dev or --sys.", arg),
    }
}

