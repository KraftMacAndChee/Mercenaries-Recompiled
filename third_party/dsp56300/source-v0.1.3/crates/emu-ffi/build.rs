fn main() {
    let crate_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");

    // Generate the C header from Rust FFI types.
    let config = cbindgen::Config::from_file(format!("{crate_dir}/cbindgen.toml"))
        .expect("failed to read cbindgen.toml");
    cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("cbindgen failed to generate header")
        .write_to_file(format!("{crate_dir}/include/dsp56300.h"));

    // Compile the C quickstart example into a static lib so it can be called
    // from the Rust test suite, verifying the C API works as documented.
    cc::Build::new()
        .file("examples/quickstart.c")
        .include("include")
        .compile("dsp56300_quickstart");
}
