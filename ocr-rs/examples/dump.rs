// cargo run --release --example dump -- <width> <height> < raw.rgba
use std::io::Read;
fn main() {
    let a: Vec<u32> = std::env::args().skip(1).map(|s| s.parse().unwrap()).collect();
    let mut buf = Vec::new();
    std::io::stdin().read_to_end(&mut buf).unwrap();
    let s = unsafe { qview_ocr::qvocr_recognize_rgba(buf.as_ptr(), a[0], a[1], a[0] * 4) };
    println!("{}", unsafe { std::ffi::CStr::from_ptr(s) }.to_str().unwrap());
    unsafe { qview_ocr::qvocr_free_string(s) };
}
