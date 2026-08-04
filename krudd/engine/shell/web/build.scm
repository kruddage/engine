; SPDX-License-Identifier: GPL-2.0-or-later
;;! The browser shell. Its deliverables are static files the generator copies
;;! next to index.html (the PWA manifest, the service worker, the icons), so it
;;! builds no libraries or executables. What is generated rather than copied are
;;! the two files that have to know the build hash, whose @VAR@ placeholders are
;;! substituted the same way core/version.h.in's are — declared here so an edit
;;! to either re-runs the generator like an edit to any other codegen input.
;;!
;;! shell.html.in needs it to resolve the renamed index.wasm through its
;;! Module.locateFile hook. sw.js.in needs it for two things at once: to name a
;;! cache that a new deploy therefore does not inherit, and — because a service
;;! worker is only reinstalled when its bytes change, and this one's *name*
;;! never may — to be a different file per build at all.
((configure-file "shell.html.in" "shell.html")
 (configure-file "sw.js.in" "sw.js"))
