// SPDX-License-Identifier: GPL-2.0-or-later

//! `serve` — build, then hand `dist/` to a browser.
//!
//! A hundred lines of `std::net` rather than a dependency, for one reason
//! beyond xtask's no-dependency rule: `application/wasm` is not optional.
//! `WebAssembly.instantiateStreaming` rejects any other content type, and the
//! usual one-liners (`python3 -m http.server`) get it wrong on some
//! platforms — which surfaces as a confusing "incorrect response MIME type"
//! rather than as a server problem. Owning the table is cheaper than
//! documenting the workaround.

use std::io::{BufRead, BufReader, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::{Component, Path, PathBuf};

use crate::Options;
use crate::web;

/// Builds and serves, blocking until interrupted. What `cargo xtask serve`
/// runs.
pub fn run(opts: &Options) -> Result<(), String> {
    let artifacts = web::build(opts)?;
    let address = format!("{}:{}", opts.host, opts.port);
    let listener = TcpListener::bind(&address).map_err(|e| {
        format!("could not bind {address}: {e}\n  is something already on that port? try `--port`")
    })?;

    println!(
        "\nserving {} at http://{address}/",
        artifacts.dist.display()
    );
    println!("ctrl-c to stop");

    serve_forever(listener, artifacts.dist);
    Ok(())
}

/// Serves an already-built `dist` on an OS-assigned loopback port, in a
/// background thread, and returns the address it bound to.
///
/// `render-test` needs a URL to point a browser at, not a blocking accept
/// loop — and an OS-assigned port means it never collides with a
/// contributor's own `cargo xtask serve` on the fixed default port, or with a
/// second CI job on the same runner.
///
/// The thread is never joined. That is not an oversight: xtask is a
/// short-lived CLI, and the server has no work left to finish when the
/// process exits — it dies with it, the same as any other child work a CLI
/// invocation leaves running.
pub fn spawn(dist: PathBuf, bind_addr: &str) -> Result<SocketAddr, String> {
    let listener =
        TcpListener::bind(bind_addr).map_err(|e| format!("could not bind {bind_addr}: {e}"))?;
    let addr = listener
        .local_addr()
        .map_err(|e| format!("could not read the bound address: {e}"))?;
    std::thread::spawn(move || serve_forever(listener, dist));
    Ok(addr)
}

/// The accept loop shared by both entry points above.
fn serve_forever(listener: TcpListener, dist: PathBuf) {
    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let dist = dist.clone();
                // A thread per connection: browsers open several at once, and
                // a sequential loop would stall the page on its own requests.
                std::thread::spawn(move || {
                    if let Err(e) = handle(stream, &dist) {
                        eprintln!("serve: {e}");
                    }
                });
            }
            Err(e) => eprintln!("serve: accept failed: {e}"),
        }
    }
}

/// Serves one request and closes the connection.
fn handle(mut stream: TcpStream, dist: &Path) -> Result<(), String> {
    let mut reader = BufReader::new(
        stream
            .try_clone()
            .map_err(|e| format!("could not read the request: {e}"))?,
    );
    let mut request_line = String::new();
    reader
        .read_line(&mut request_line)
        .map_err(|e| format!("could not read the request line: {e}"))?;

    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or("");
    let target = parts.next().unwrap_or("/");

    // Only HEAD suppresses the body. Passing `true` here unconditionally
    // would send a Content-Length with nothing behind it, which a browser
    // reports as ERR_CONTENT_LENGTH_MISMATCH rather than as a 404.
    let head_only = method == "HEAD";
    if method != "GET" && !head_only {
        return respond(
            &mut stream,
            405,
            "text/plain",
            b"method not allowed",
            head_only,
        );
    }

    let path = target.split(['?', '#']).next().unwrap_or("/");
    let Some(file) = resolve(dist, path) else {
        return respond(&mut stream, 404, "text/plain", b"not found", head_only);
    };

    let body = std::fs::read(&file).map_err(|e| format!("{}: {e}", file.display()))?;
    println!(
        "  {method} {path} -> {} ({} bytes)",
        file.display(),
        body.len()
    );
    respond(&mut stream, 200, content_type(&file), &body, head_only)
}

/// Maps a request path onto a file inside `dist`, or `None` if it escapes or
/// does not exist.
fn resolve(dist: &Path, path: &str) -> Option<PathBuf> {
    let relative = path.trim_start_matches('/');
    let relative = if relative.is_empty() {
        "index.html"
    } else {
        relative
    };

    let candidate = dist.join(relative);
    // Reject traversal before touching the filesystem. `..` in the request is
    // the only way out of dist/, and a dev server that will hand over
    // ~/.ssh/id_rsa is a dev server that gets left running.
    if candidate
        .components()
        .any(|c| matches!(c, Component::ParentDir))
    {
        return None;
    }
    if candidate.is_dir() {
        let index = candidate.join("index.html");
        return index.is_file().then_some(index);
    }
    candidate.is_file().then_some(candidate)
}

/// The content type for a file extension.
///
/// `application/wasm` is the one that must be right; the rest are here so a
/// page does not have to work around the server.
fn content_type(path: &Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()) {
        Some("html") => "text/html; charset=utf-8",
        Some("js" | "mjs") => "text/javascript; charset=utf-8",
        Some("wasm") => "application/wasm",
        Some("json" | "map") => "application/json; charset=utf-8",
        Some("css") => "text/css; charset=utf-8",
        Some("svg") => "image/svg+xml",
        Some("png") => "image/png",
        Some("ico") => "image/vnd.microsoft.icon",
        Some("webmanifest") => "application/manifest+json",
        _ => "application/octet-stream",
    }
}

/// Writes a complete response and closes.
fn respond(
    stream: &mut TcpStream,
    status: u16,
    content_type: &str,
    body: &[u8],
    head_only: bool,
) -> Result<(), String> {
    let reason = match status {
        200 => "OK",
        404 => "Not Found",
        405 => "Method Not Allowed",
        _ => "Error",
    };
    let header = format!(
        "HTTP/1.1 {status} {reason}\r\n\
         Content-Type: {content_type}\r\n\
         Content-Length: {}\r\n\
         Cache-Control: no-store\r\n\
         Connection: close\r\n\
         \r\n",
        body.len()
    );
    stream
        .write_all(header.as_bytes())
        .map_err(|e| format!("could not write the response: {e}"))?;
    if !head_only {
        stream
            .write_all(body)
            .map_err(|e| format!("could not write the body: {e}"))?;
    }
    stream
        .flush()
        .map_err(|e| format!("could not flush the response: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A `dist` with one file in it, in a fresh temporary directory.
    fn fixture() -> PathBuf {
        let dir = std::env::temp_dir().join(format!("krudd-serve-{}", std::process::id()));
        std::fs::create_dir_all(&dir).expect("temp dir");
        std::fs::write(dir.join("index.html"), "<!doctype html>").expect("index.html");
        dir
    }

    #[test]
    fn the_root_resolves_to_the_index() {
        let dist = fixture();
        assert_eq!(resolve(&dist, "/"), Some(dist.join("index.html")));
    }

    #[test]
    fn a_query_string_is_not_part_of_the_path() {
        let dist = fixture();
        assert_eq!(resolve(&dist, "index.html"), Some(dist.join("index.html")));
    }

    #[test]
    fn a_missing_file_resolves_to_nothing() {
        assert_eq!(resolve(&fixture(), "/nope.js"), None);
    }

    #[test]
    fn traversal_out_of_dist_is_refused() {
        // The target genuinely exists on every machine this runs on, so a
        // resolver that only checked `is_file` would happily serve it.
        let dist = fixture();
        assert_eq!(resolve(&dist, "/../../../../etc/passwd"), None);
        assert_eq!(resolve(&dist, "/a/../../etc/passwd"), None);
    }

    #[test]
    fn wasm_gets_its_own_content_type() {
        // The one that must be right: instantiateStreaming rejects anything
        // else, and reports it as a page error rather than a server one.
        assert_eq!(
            content_type(Path::new("krudd_web_bg.wasm")),
            "application/wasm"
        );
    }

    #[test]
    fn an_unknown_extension_falls_back_rather_than_guessing() {
        assert_eq!(
            content_type(Path::new("thing.krudd")),
            "application/octet-stream"
        );
    }
}
