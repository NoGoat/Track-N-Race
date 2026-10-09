use std::{env, path::PathBuf};

fn main() {
    println!("cargo:rerun-if-changed=native");
    println!("cargo:rerun-if-changed=../../protocol_parser_library");
    // Cargo's OUT_DIR nests target/profile/build/package-hash/out. MSBuild's
    // file tracker still hits MAX_PATH in CMake's compiler probes there.
    // Keep native output shallow, and isolate architectures and Cargo profiles.
    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let native_out = manifest_dir
        .parent()
        .expect("src-tauri must have a frontend directory")
        .join(".native")
        .join(env::var("TARGET").unwrap())
        .join(env::var("PROFILE").unwrap());
    let dst = cmake::Config::new("native")
        .out_dir(native_out)
        .profile("Release")
        .define("CMAKE_POSITION_INDEPENDENT_CODE", "ON")
        .build();
    println!("cargo:rustc-link-search=native={}/lib", dst.display());
    for name in ["tnr_tauri_core", "tnrp", "tnr_xlsx", "tnr_zstd", "tnr_zlib"] {
        println!("cargo:rustc-link-lib=static={name}");
    }
    let target = std::env::var("CARGO_CFG_TARGET_OS").unwrap();
    match target.as_str() {
        "windows" => println!("cargo:rustc-link-lib=ws2_32"),
        "macos" => println!("cargo:rustc-link-lib=c++"),
        _ => {
            println!("cargo:rustc-link-lib=stdc++");
            println!("cargo:rustc-link-lib=pthread");
            println!("cargo:rustc-link-lib=m");
        }
    }
    tauri_build::build();
}
