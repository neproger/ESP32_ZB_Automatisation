use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    embuild::espidf::sysenv::output();
    generate_lvgl_bindings();
}

/// LVGL is a managed IDF component; generate Rust bindings against exactly the
/// headers/config of the component that the C side compiles.
fn generate_lvgl_bindings() {
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let idf_path = PathBuf::from(env::var("IDF_PATH").unwrap_or_else(|_| {
        "C:/Users/ASUS/.espressif/esp-idf/v6.1".to_string()
    }));

    let Some(sys_out) = find_sys_out(&out_dir) else {
        panic!("esp-idf-sys out dir not found; run the build through tools/build-ha-app.ps1");
    };
    let lvgl = sys_out.join("managed_components/lvgl__lvgl");
    if !lvgl.join("lvgl.h").exists() {
        panic!("LVGL component not found at {}", lvgl.display());
    }

    let header = manifest.join("native_display/include/ha_display.h");
    let mut builder = bindgen::Builder::default()
        .header(header.to_string_lossy())
        // Match the firmware configuration: ESP_PLATFORM pulls sdkconfig through
        // lv_conf_kconfig.h, so struct layouts (lv_color_t) are identical.
        .clang_arg("-DESP_PLATFORM")
        .clang_arg("-Wno-everything")
        .clang_arg(format!("-I{}", lvgl.display()))
        .clang_arg(format!("-I{}", lvgl.join("src").display()))
        .clang_arg(format!("-I{}", sys_out.join("build/config").display()))
        .clang_arg(format!("-I{}", idf_path.join("components/esp_common/include").display()))
        .layout_tests(false)
        .generate_comments(false)
        .derive_default(true);
    // libclang ships without C headers on Windows; use the ESP RISC-V GCC ones.
    for include in riscv_gcc_includes() {
        builder = builder.clang_arg(format!("-I{}", include.display()));
    }
    let bindings = builder.generate().expect("generate LVGL bindings");
    bindings
        .write_to_file(out_dir.join("lvgl_bindings.rs"))
        .expect("write LVGL bindings");

    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-changed={}", lvgl.join("lvgl.h").display());
}

fn find_sys_out(out_dir: &Path) -> Option<PathBuf> {
    let mut dir = out_dir;
    loop {
        let parent = dir.parent()?;
        if let Ok(entries) = fs::read_dir(parent) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir()
                    && path
                        .file_name()
                        .is_some_and(|name| name.to_string_lossy().starts_with("esp-idf-sys-"))
                {
                    let out = path.join("out");
                    if out.join("managed_components").is_dir() {
                        return Some(out);
                    }
                }
            }
        }
        dir = parent;
    }
}

/// Headers of the ESP RISC-V GCC toolchain (libclang has no built-in C headers).
fn riscv_gcc_includes() -> Vec<PathBuf> {
    let tools = PathBuf::from(
        env::var("IDF_TOOLS_PATH").unwrap_or_else(|_| "C:/Users/ASUS/.espressif".to_string()),
    );
    let Ok(entries) = fs::read_dir(tools.join("tools/riscv32-esp-elf")) else {
        return Vec::new();
    };
    let Some(root) = entries
        .flatten()
        .map(|entry| entry.path())
        .find(|path| path.is_dir() && path.join("riscv32-esp-elf").is_dir())
    else {
        return Vec::new();
    };
    let root = root.join("riscv32-esp-elf");
    let mut out = vec![root.join("riscv32-esp-elf/include")];
    if let Ok(versions) = fs::read_dir(root.join("lib/gcc/riscv32-esp-elf")) {
        if let Some(version) = versions.flatten().map(|entry| entry.path()).find(|p| p.is_dir()) {
            out.push(version.join("include"));
        }
    }
    out
}
