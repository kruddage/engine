; SPDX-License-Identifier: GPL-2.0-or-later
;;! The browser shell. Its deliverables are static files the generator copies
;;! next to index.html (the PWA manifest, the service worker, the icons), so it
;;! builds no libraries or executables. The one thing that is generated rather
;;! than copied is the emscripten shell template, whose @VAR@ placeholders are
;;! substituted the same way core/version.h.in's are — declared here so an edit
;;! to it re-runs the generator like an edit to any other codegen input.
((configure-file "shell.html.in" "shell.html"))
