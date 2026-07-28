// SPDX-License-Identifier: GPL-2.0-or-later
//
// The editor's entry point.

import { StrictMode } from "react";
import { createRoot } from "react-dom/client";

import { App } from "./app.js";
import "./styles.css";

const host = document.getElementById("root");
if (host === null) throw new Error("index.html is missing #root");

createRoot(host).render(
	<StrictMode>
		<App />
	</StrictMode>
);
