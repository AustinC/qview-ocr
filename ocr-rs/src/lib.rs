//! Tiny C ABI around the `ocrs` OCR engine for qView.
//!
//! The C++ side hands us decoded RGBA pixels (so any format Qt can read
//! works) and gets back a JSON string describing every recognised line and
//! the bounding box of each character, in image pixel coordinates.

use std::ffi::{CString, c_char};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use anyhow::{Context, Result, anyhow};
use ocrs::{ImageSource, OcrEngine, OcrEngineParams, TextItem};
use rten::Model;
use serde::Serialize;

const MODEL_BASE_URL: &str = "https://ocrs-models.s3-accelerate.amazonaws.com";
const DETECTION_MODEL: &str = "text-detection.onnx";
const RECOGNITION_MODEL: &str = "text-recognition.onnx";

#[derive(Serialize)]
struct Char {
    c: char,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
}

#[derive(Serialize)]
struct Line {
    chars: Vec<Char>,
}

#[derive(Serialize)]
#[serde(untagged)]
enum Output {
    Ok { lines: Vec<Line> },
    Err { error: String },
}

fn model_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os("QVIEW_OCR_MODELS") {
        return PathBuf::from(dir);
    }
    let cache = std::env::var_os("XDG_CACHE_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| Path::new(&h).join(".cache")))
        .unwrap_or_else(std::env::temp_dir);
    cache.join("ocrs")
}

/// Return the path to `name`, downloading it on first use.
fn ensure_model(name: &str) -> Result<PathBuf> {
    let dir = model_dir();
    let path = dir.join(name);
    if path.exists() {
        return Ok(path);
    }
    fs::create_dir_all(&dir).with_context(|| format!("creating {}", dir.display()))?;
    let url = format!("{MODEL_BASE_URL}/{name}");
    eprintln!("qview-ocr: downloading {url}");
    let resp = ureq::get(&url).call().with_context(|| format!("GET {url}"))?;
    let tmp = path.with_extension("part");
    let mut file = fs::File::create(&tmp)?;
    io::copy(&mut resp.into_body().into_reader(), &mut file)?;
    fs::rename(&tmp, &path)?;
    Ok(path)
}

fn build_engine() -> Result<OcrEngine> {
    let detection = Model::load_file(ensure_model(DETECTION_MODEL)?)?;
    let recognition = Model::load_file(ensure_model(RECOGNITION_MODEL)?)?;
    OcrEngine::new(OcrEngineParams {
        detection_model: Some(detection),
        recognition_model: Some(recognition),
        ..Default::default()
    })
}

fn engine() -> Result<&'static OcrEngine> {
    static ENGINE: OnceLock<Result<OcrEngine, String>> = OnceLock::new();
    ENGINE
        .get_or_init(|| build_engine().map_err(|e| format!("{e:#}")))
        .as_ref()
        .map_err(|e| anyhow!("{e}"))
}

fn recognize(rgba: &[u8], width: u32, height: u32) -> Result<Vec<Line>> {
    let engine = engine()?;
    let input = engine.prepare_input(ImageSource::from_bytes(rgba, (width, height))?)?;
    let words = engine.detect_words(&input)?;
    let line_rects = engine.find_text_lines(&input, &words);
    let lines = engine
        .recognize_text(&input, &line_rects)?
        .into_iter()
        .flatten()
        // Drop single-glyph noise, as the ocrs examples do.
        .filter(|l| l.chars().iter().filter(|c| c.char != ' ').count() > 1)
        .map(|l| Line {
            chars: l
                .chars()
                .iter()
                .map(|c| Char {
                    c: c.char,
                    x: c.rect.left(),
                    y: c.rect.top(),
                    w: c.rect.width(),
                    h: c.rect.height(),
                })
                .collect(),
        })
        .collect();
    Ok(lines)
}

/// Run OCR on an RGBA8888 image.
///
/// Returns a NUL-terminated JSON string which must be released with
/// [`qvocr_free_string`]. On success it is `{"lines":[{"chars":[{c,x,y,w,h}..]}..]}`,
/// on failure `{"error":"..."}`. Blocking; call from a worker thread.
///
/// # Safety
/// `rgba` must point to `height` rows of `stride` bytes, each holding at
/// least `width * 4` bytes of pixel data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qvocr_recognize_rgba(
    rgba: *const u8,
    width: u32,
    height: u32,
    stride: u32,
) -> *mut c_char {
    let result = std::panic::catch_unwind(|| {
        if rgba.is_null() || width == 0 || height == 0 || (stride as u64) < width as u64 * 4 {
            return Err(anyhow!("invalid image buffer"));
        }
        let row = width as usize * 4;
        // Pack rows tightly; ocrs expects contiguous HWC data.
        let mut packed = Vec::with_capacity(row * height as usize);
        for y in 0..height as usize {
            let src = unsafe { std::slice::from_raw_parts(rgba.add(y * stride as usize), row) };
            packed.extend_from_slice(src);
        }
        recognize(&packed, width, height)
    });
    let out = match result {
        Ok(Ok(lines)) => Output::Ok { lines },
        Ok(Err(e)) => Output::Err { error: format!("{e:#}") },
        Err(_) => Output::Err { error: "OCR panicked".into() },
    };
    let json = serde_json::to_string(&out).unwrap_or_else(|_| r#"{"error":"serialize"}"#.into());
    CString::new(json).unwrap_or_default().into_raw()
}

/// Free a string returned by [`qvocr_recognize_rgba`].
///
/// # Safety
/// `s` must come from `qvocr_recognize_rgba` and not have been freed already.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qvocr_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}
