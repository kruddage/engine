/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * krudd_qt — the native Vulkan backend, presenting into a Qt-hosted window.
 *
 * The native editor (see docs/qt-editor-shell.md and #675/#705): the engine's
 * Vulkan backend, through the vulkan_platform.h seam, driving a QWindow
 * embedded in real editor chrome. A QMainWindow owns the window and hands the
 * backend its VkSurfaceKHR; the viewport is a QWindow embedded via
 * QWidget::createWindowContainer. That embedded QWindow is the "canvas is a
 * window" viewport #675 asks for.
 *
 * WHAT RENDERS (see #705 and renderer_vulkan.c's SCOPE note): after the device
 * lands this boots the engine's render cluster natively (editor_boot_cluster —
 * asset, entity, frame_graph, scene_renderer) exactly as the web boot does, so
 * the scene renderer records its built-in demo scene into the gpu_api. The
 * Vulkan backend does not yet translate that draw stream — pipelines, buffers
 * and draws are stubbed — so the viewport currently shows the backend's
 * animated clear rather than the scene. The point of this pass is the
 * *validated Vulkan base*: window -> VkSurfaceKHR -> a modern Vulkan 1.3 device
 * with the Khronos validation layers on -> swapchain -> present, so the real
 * forward pass is built and debugged against a loader that already talks back.
 *
 * The chrome is the authoring surface #676 asks for, laid out but not yet
 * wired: a File/Edit/View/Help menu bar like a normal desktop app, and the
 * Scene / Inspector / Assets / Console (Scheme REPL) docks around the
 * viewport. The docks are freely movable, floatable, tabbable and nestable
 * (View > Reset Layout restores the default), so the layout is the user's to
 * rearrange. Panel *contents* are "coming soon" placeholders — wiring each to
 * the running image (scene graph, inspector edits, live REPL, project
 * open/save) is the rest of #676, not this pass.
 *
 * DELIBERATELY MOC-FREE. Nothing here declares Q_OBJECT, a signal, or a
 * slot — every connection is to a Qt-supplied QObject (qApp, a QTimer, a
 * QShortcut) through a lambda, which Qt6 allows without a generated moc
 * file. That keeps the kruddmake (qt) clause's job to "add -I/-l flags and
 * compile this .cpp with a C++ compiler" — the same shape as every other
 * native target, with no moc rule to add to ninja.scm.
 *
 * SURFACE CREATION, PER PLATFORM. The window handles come from Qt's stable
 * public native-interface API, and become a VkSurfaceKHR through the matching
 * VK_KHR_*_surface entry point:
 *   - Wayland (the Deck's primary path): QNativeInterface::QWaylandWindow /
 *     QWaylandApplication give the wl_surface / wl_display —
 *     vkCreateWaylandSurfaceKHR. This needs Qt >= 6.5 (that native interface was
 *     added then; pre-6.5 has no stable public route and the build #errors).
 *   - X11/XWayland: QNativeInterface::QX11Application::connection() gives the
 *     xcb_connection_t*, QWindow::winId() the window — vkCreateXcbSurfaceKHR.
 *   - Windows: QWindow::winId() is the HWND — vkCreateWin32SurfaceKHR.
 * The VK_USE_PLATFORM_* selection below deliberately avoids pulling the heavy
 * native headers (Xlib.h, wayland-client.h): the Wayland/XCB Vulkan structs
 * take pointers to incomplete types plus the one xcb_window_t alias, so this
 * TU stays free of X11/Wayland macro pollution that would fight Qt's headers.
 */

/* Select the WSI platform(s) before <vulkan/vulkan.h> is first pulled in
 * (through vulkan_platform.h), so the matching VkCreate*SurfaceKHR entry points
 * and structs are declared. xcb_window_t is the only native typedef needed —
 * the xcb/wayland structs otherwise take pointers to incomplete types — so we
 * alias it here rather than include <xcb/xcb.h> and its macro baggage. */
#include <stdint.h>
#if defined(_WIN32)
/* vulkan_win32.h (pulled in below with the macro set) needs the Win32 types, so
 * windows.h must precede it. */
#  include <windows.h>
#  define VK_USE_PLATFORM_WIN32_KHR
#else
#  define VK_USE_PLATFORM_WAYLAND_KHR
#  define VK_USE_PLATFORM_XCB_KHR
typedef uint32_t xcb_window_t;
#endif

extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log.h"
#include "log_api.h"
#include "memory.h"
#include "memory_api.h"
#include "stats_api.h"       /* entity reads the frame delta off "stats" */
#include "script.h"
#include "renderer.h"        /* struct gpu_api — the backend's vtable */
#include "camera_api.h"      /* set the projection aspect to the viewport */
#include "editor_boot.h"     /* the native render-cluster boot */
#include "vulkan_platform.h" /* the native windowing host seam (VkSurfaceKHR) */
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QShortcut>
#include <QtGui/QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QWidget>

extern "C" {
void renderer_vulkan_plugin_entry(struct subsystem_manager *mgr);
int  renderer_vulkan_device_ready(void);
}

/* Mirrors engine.c's core service table — the same pair engine_native.c
 * stands up, static to each Emscripten-only-adjacent TU because it cannot be
 * shared across them. */
static const struct log_api g_log_api = {
	.write       = log_write,
	.get_history = log_get_history,
};

static const struct memory_api g_mem_api = {
	.alloc        = mem_alloc,
	.alloc_zero   = mem_alloc_zero,
	.free         = mem_free,
	.pool_create  = mem_pool_create,
	.pool_alloc   = mem_pool_alloc,
	.pool_free    = mem_pool_free,
	.pool_destroy = mem_pool_destroy,
};

/*
 * Mutable, updated each frame with the real inter-frame delta: the entity
 * subsystem reads last_frame_ms off "stats" to advance the seeded scene's
 * scripts (the spinner, the orbit camera), the same way the browser feeds it
 * the rAF delta. Zero here would freeze the scene on its rest pose.
 */
static struct stats_api g_stats_api;

static const struct subsystem subsystems[] = {
	{ .name = "log",    .api = &g_log_api, .init = log_init, .shutdown = log_shutdown },
	{ .name = "memory", .api = &g_mem_api, .init = mem_init, .shutdown = mem_shutdown },
	{ .name = "stats",  .api = &g_stats_api                                           },
	{ }
};

static struct subsystem_manager manager;

/* ------------------------------------------------------- the viewport ---- */

/*
 * The embedded native surface. A plain QWindow subclass — no Q_OBJECT, see
 * the file comment — set to VulkanSurface so QPA does not install its own
 * GL-backed backing store over a window the Vulkan backend presents into. This
 * is the honest enumerator now that the native backend is Vulkan directly:
 * VulkanSurface on every platform it targets (Vulkan on Linux/the Deck, and
 * Vulkan on Windows).
 */
class KruddViewportWindow : public QWindow {
public:
	KruddViewportWindow() { setSurfaceType(QSurface::VulkanSurface); }
};

/*
 * Build the VkSurfaceKHR for the embedded viewport window. Called by the
 * backend through the vulkan_platform host, once, at device bring-up. The
 * per-platform branches mirror the VK_USE_PLATFORM_* selection at the top of
 * the file; if a future platform is added, this function and that selection are
 * the two places to touch.
 */
static VkSurfaceKHR window_create_surface(VkInstance instance, void *user)
{
	KruddViewportWindow *win = static_cast<KruddViewportWindow *>(user);
	const QString platform = QGuiApplication::platformName();
	VkSurfaceKHR surface = VK_NULL_HANDLE;

#if defined(VK_USE_PLATFORM_WIN32_KHR)
	if (platform == QLatin1String("windows")) {
		VkWin32SurfaceCreateInfoKHR ci;

		memset(&ci, 0, sizeof(ci));
		ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		ci.hinstance = GetModuleHandle(nullptr);
		ci.hwnd      = reinterpret_cast<HWND>(win->winId());
		if (vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface)
		    != VK_SUCCESS)
			return VK_NULL_HANDLE;
		return surface;
	}
#endif

#if defined(VK_USE_PLATFORM_XCB_KHR)
	if (platform == QLatin1String("xcb")) {
		auto *x11app =
			qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
		VkXcbSurfaceCreateInfoKHR ci;

		if (!x11app || !x11app->connection()) {
			fprintf(stderr, "krudd_qt: no X11/xcb connection\n");
			return VK_NULL_HANDLE;
		}
		memset(&ci, 0, sizeof(ci));
		ci.sType      = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
		ci.connection = x11app->connection();
		ci.window     = static_cast<xcb_window_t>(win->winId());
		if (vkCreateXcbSurfaceKHR(instance, &ci, nullptr, &surface)
		    != VK_SUCCESS)
			return VK_NULL_HANDLE;
		return surface;
	}
#endif

#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
	if (platform == QLatin1String("wayland")) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		auto *wlwin =
			win->nativeInterface<QNativeInterface::QWaylandWindow>();
		auto *wlapp =
			qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
		VkWaylandSurfaceCreateInfoKHR ci;

		if (!wlwin || !wlapp || !wlwin->surface() || !wlapp->display()) {
			fprintf(stderr, "krudd_qt: no Wayland handles\n");
			return VK_NULL_HANDLE;
		}
		memset(&ci, 0, sizeof(ci));
		ci.sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
		ci.display = wlapp->display();
		ci.surface = wlwin->surface();
		if (vkCreateWaylandSurfaceKHR(instance, &ci, nullptr, &surface)
		    != VK_SUCCESS)
			return VK_NULL_HANDLE;
		return surface;
#else
#error "krudd_qt: Wayland needs QNativeInterface::QWaylandWindow, added in " \
       "Qt 6.5. Pre-6.5 Qt has no stable public way to reach a QWindow's " \
       "wl_surface (see the file comment) — build with Qt >= 6.5, or add " \
       "and own a private-header fallback deliberately rather than by " \
       "accident here."
#endif
	}
#endif

	fprintf(stderr,
		"krudd_qt: unsupported Qt platform plugin '%s' "
		"(want windows, xcb or wayland)\n",
		qPrintable(platform));
	return VK_NULL_HANDLE;
}

/* Physical (device) pixels: the backend configures the surface in physical
 * pixels, and QWindow::size() is in device-independent pixels, so scale by the window's
 * own devicePixelRatio rather than the application-wide default — right on
 * a mixed-DPI multi-monitor setup where the window has actually landed. */
static void window_drawable_size(uint32_t *w, uint32_t *h, void *user)
{
	KruddViewportWindow *win = static_cast<KruddViewportWindow *>(user);
	qreal dpr = win->devicePixelRatio();
	int   pw  = static_cast<int>(win->width() * dpr);
	int   ph  = static_cast<int>(win->height() * dpr);

	*w = (pw > 0) ? static_cast<uint32_t>(pw) : 1u;
	*h = (ph > 0) ? static_cast<uint32_t>(ph) : 1u;
}

/* ------------------------------------------------------------- the shell */

/*
 * The authoring surface #676 asks for: real editor chrome — a File/Edit/
 * View/Help menu bar, a toolbar, and Scene / Inspector / Assets / Console
 * docks around the embedded viewport. Panel contents are "coming soon"
 * placeholders (see the file comment); the point of this pass is the shell —
 * a normal desktop menu bar and docks the user can freely rearrange.
 */

/* A centred "coming soon" placeholder body for a not-yet-wired panel: the
 * panel's real name, a "Coming soon" line, and one sentence on what it will
 * become, so the empty layout still explains itself. */
static QWidget *make_placeholder(const QString &title, const QString &blurb)
{
	QLabel *label = new QLabel(
		QStringLiteral("<div align='center'>"
			       "<p style='font-size:15px'><b>%1</b></p>"
			       "<p style='color:gray'>Coming soon</p>"
			       "<p style='color:gray'>%2</p></div>")
			.arg(title.toHtmlEscaped(), blurb.toHtmlEscaped()));
	label->setAlignment(Qt::AlignCenter);
	label->setWordWrap(true);
	label->setMargin(12);
	return label;
}

/* Wrap a panel body in a QDockWidget with a stable objectName (so
 * saveState/restoreState round-trips it — that is what View > Reset Layout
 * relies on) and full move/float/close freedom in every dock area. */
static QDockWidget *make_dock(const QString &title, const QString &object_name,
			      QWidget *body, QMainWindow *win)
{
	QDockWidget *dock = new QDockWidget(title, win);

	dock->setObjectName(object_name);
	dock->setWidget(body);
	dock->setAllowedAreas(Qt::AllDockWidgetAreas);
	dock->setFeatures(QDockWidget::DockWidgetMovable |
			  QDockWidget::DockWidgetFloatable |
			  QDockWidget::DockWidgetClosable);
	return dock;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setApplicationName(QStringLiteral("krudd"));

	QMainWindow win;
	win.setObjectName(QStringLiteral("KruddEditorWindow"));
	win.setWindowTitle(QStringLiteral("krudd — editor"));

	/* Docks the user can nest, tab and grouped-drag into any arrangement —
	 * the "fully reconfigurable" authoring surface #676 asks for. */
	win.setDockNestingEnabled(true);
	win.setDockOptions(QMainWindow::AnimatedDocks |
			   QMainWindow::AllowNestedDocks |
			   QMainWindow::AllowTabbedDocks |
			   QMainWindow::GroupedDragging);

	/* Panels are placeholders for now (#676), so every menu action that is
	 * not wired yet says so in the status bar rather than doing nothing —
	 * the honest desktop-app hint, without a modal on every click. */
	auto coming_soon = [&win](const QString &what) {
		win.statusBar()->showMessage(
			what + QStringLiteral(" — coming soon"), 4000);
	};

	/* A menu action with a shortcut + lambda, spelled out the version-safe
	 * way (build the QAction, set the shortcut, connect) so no Qt 6.3+-only
	 * addAction overload is required. */
	auto add_action = [](QMenu *menu, const QString &text,
			     const QKeySequence &shortcut,
			     std::function<void()> fn) {
		QAction *a = menu->addAction(text);

		if (!shortcut.isEmpty())
			a->setShortcut(shortcut);
		QObject::connect(a, &QAction::triggered, fn);
		return a;
	};

	/* ---- File ------------------------------------------------------- */
	QMenu *file_menu = win.menuBar()->addMenu(QStringLiteral("&File"));
	add_action(file_menu, QStringLiteral("&New Project"), QKeySequence::New,
		   [=]() { coming_soon(QStringLiteral("New Project")); });
	add_action(file_menu, QStringLiteral("&Open Project…"),
		   QKeySequence::Open,
		   [=]() { coming_soon(QStringLiteral("Open Project")); });
	file_menu->addSeparator();
	add_action(file_menu, QStringLiteral("&Save"), QKeySequence::Save,
		   [=]() { coming_soon(QStringLiteral("Save")); });
	add_action(file_menu, QStringLiteral("Save &As…"), QKeySequence::SaveAs,
		   [=]() { coming_soon(QStringLiteral("Save As")); });
	file_menu->addSeparator();
	file_menu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit,
			     &app, &QApplication::quit);

	/* ---- Edit ------------------------------------------------------- */
	QMenu *edit_menu = win.menuBar()->addMenu(QStringLiteral("&Edit"));
	add_action(edit_menu, QStringLiteral("&Undo"), QKeySequence::Undo,
		   [=]() { coming_soon(QStringLiteral("Undo")); });
	add_action(edit_menu, QStringLiteral("&Redo"), QKeySequence::Redo,
		   [=]() { coming_soon(QStringLiteral("Redo")); });
	edit_menu->addSeparator();
	add_action(edit_menu, QStringLiteral("Cu&t"), QKeySequence::Cut,
		   [=]() { coming_soon(QStringLiteral("Cut")); });
	add_action(edit_menu, QStringLiteral("&Copy"), QKeySequence::Copy,
		   [=]() { coming_soon(QStringLiteral("Copy")); });
	add_action(edit_menu, QStringLiteral("&Paste"), QKeySequence::Paste,
		   [=]() { coming_soon(QStringLiteral("Paste")); });

	/* ---- the authoring docks (real layout, placeholder contents) ---- */
	QDockWidget *scene_dock = make_dock(
		QStringLiteral("Scene"), QStringLiteral("dock.scene"),
		make_placeholder(QStringLiteral("Scene Tree"),
			QStringLiteral("The entity hierarchy of the open "
				"project — pick a node to edit it in the "
				"Inspector.")),
		&win);
	win.addDockWidget(Qt::LeftDockWidgetArea, scene_dock);

	QDockWidget *inspector_dock = make_dock(
		QStringLiteral("Inspector"), QStringLiteral("dock.inspector"),
		make_placeholder(QStringLiteral("Inspector"),
			QStringLiteral("Components and properties of the "
				"selected entity, written back to the project "
				"files.")),
		&win);
	win.addDockWidget(Qt::RightDockWidgetArea, inspector_dock);

	QDockWidget *assets_dock = make_dock(
		QStringLiteral("Assets"), QStringLiteral("dock.assets"),
		make_placeholder(QStringLiteral("Asset Browser"),
			QStringLiteral("Meshes, textures, sounds and scenes in "
				"the project, ready to drag into the scene.")),
		&win);
	win.addDockWidget(Qt::BottomDockWidgetArea, assets_dock);

	QDockWidget *console_dock = make_dock(
		QStringLiteral("Console"), QStringLiteral("dock.console"),
		make_placeholder(QStringLiteral("Scheme REPL"),
			QStringLiteral("A live S7 Scheme console into the "
				"running engine image — evaluate against the "
				"game as it plays.")),
		&win);
	win.addDockWidget(Qt::BottomDockWidgetArea, console_dock);
	win.tabifyDockWidget(assets_dock, console_dock);
	assets_dock->raise();

	/* ---- View (dock toggles + reset) -------------------------------- */
	QMenu *view_menu = win.menuBar()->addMenu(QStringLiteral("&View"));
	view_menu->addAction(scene_dock->toggleViewAction());
	view_menu->addAction(inspector_dock->toggleViewAction());
	view_menu->addAction(assets_dock->toggleViewAction());
	view_menu->addAction(console_dock->toggleViewAction());
	view_menu->addSeparator();

	/* Filled in once the window is shown (below). The reset action restores
	 * that snapshot, so the user can always get the default layout back
	 * after dragging the docks around. */
	QByteArray default_geometry;
	QByteArray default_state;
	QAction *reset_layout =
		view_menu->addAction(QStringLiteral("Reset &Layout"));
	QObject::connect(reset_layout, &QAction::triggered, [&]() {
		if (!default_geometry.isEmpty())
			win.restoreGeometry(default_geometry);
		if (!default_state.isEmpty())
			win.restoreState(default_state);
	});

	/* ---- Help ------------------------------------------------------- */
	QMenu *help_menu = win.menuBar()->addMenu(QStringLiteral("&Help"));
	help_menu->addAction(QStringLiteral("&About krudd"), [&win]() {
		QMessageBox::about(&win, QStringLiteral("About krudd"),
			QStringLiteral(
				"<b>krudd — editor</b><br><br>"
				"The Qt authoring surface (#676) around the "
				"native Vulkan viewport (#675/#705).<br><br>"
				"The centre panel is a native Vulkan "
				"surface embedded via "
				"QWidget::createWindowContainer. The docks "
				"around it — Scene, Inspector, Assets and "
				"Console — are the authoring surface: drag, "
				"float or tab them into any layout, and use "
				"View &gt; Reset Layout to restore the "
				"default."));
	});

	/* ---- toolbar ---------------------------------------------------- */
	QToolBar *toolbar = win.addToolBar(QStringLiteral("main"));
	toolbar->setObjectName(QStringLiteral("toolbar.main"));
	QAction *play_action = toolbar->addAction(QStringLiteral("▶ Play"));
	QObject::connect(play_action, &QAction::triggered,
			 [=]() { coming_soon(QStringLiteral("Play")); });
	QAction *stop_action = toolbar->addAction(QStringLiteral("■ Stop"));
	QObject::connect(stop_action, &QAction::triggered,
			 [=]() { coming_soon(QStringLiteral("Stop")); });
	toolbar->addSeparator();
	QLabel *renderer_badge = new QLabel(QStringLiteral("Vulkan — booting…"));
	toolbar->addWidget(renderer_badge);

	KruddViewportWindow *viewport = new KruddViewportWindow();
	QWidget *viewport_container =
		QWidget::createWindowContainer(viewport, &win);
	viewport_container->setMinimumSize(320, 240);
	win.setCentralWidget(viewport_container);

	QLabel *fps_readout = new QLabel(QStringLiteral("fps —"));
	QLabel *res_readout = new QLabel(QStringLiteral("—×—"));
	QLabel *driver_readout =
		new QLabel(QGuiApplication::platformName().toUpper());
	win.statusBar()->addPermanentWidget(fps_readout);
	win.statusBar()->addPermanentWidget(res_readout);
	win.statusBar()->addPermanentWidget(driver_readout);

	new QShortcut(QKeySequence(Qt::Key_Escape), &win, &QApplication::quit);

	/* Show before wiring the host: createWindowContainer's native child
	 * window is not reliably reparented/realised until the top-level is
	 * shown, and window_create_surface below needs a real native handle. */
	win.resize(1280, 800);
	win.show();

	/* Now the docks are laid out and realised, snapshot the default
	 * arrangement so View > Reset Layout can return to it. */
	default_geometry = win.saveGeometry();
	default_state    = win.saveState();

	/*
	 * Register the window host BEFORE the backend boots: renderer_vulkan_init
	 * asks the platform seam for a surface during bring-up, and the host is
	 * what turns that from "headless" into "this window".
	 */
	struct vulkan_platform_host host;

	memset(&host, 0, sizeof(host));
	host.create_surface = window_create_surface;
	host.drawable_size  = window_drawable_size;
	host.user           = viewport;
	vulkan_platform_set_host(&host);

	subsystem_manager_init(&manager, subsystems);
	script_init();
	renderer_vulkan_plugin_entry(&manager);

	/* Vulkan device bring-up is synchronous, but keep the loop (and the UI
	 * pumping) so a backend that ever needs a few ticks to settle still
	 * lands, and the window is responsive meanwhile. */
	bool ready = false;
	for (int i = 0; i < 1000 && !ready; i++) {
		subsystem_manager_tick(&manager);
		QApplication::processEvents();
		ready = renderer_vulkan_device_ready();
	}
	if (!ready) {
		fprintf(stderr, "krudd_qt: device never became ready\n");
		vulkan_platform_set_host(nullptr);
		subsystem_manager_shutdown(&manager);
		return 1;
	}

	const struct gpu_api *gpu = static_cast<const struct gpu_api *>(
		subsystem_manager_get_api(&manager, "renderer"));
	if (!gpu) {
		fprintf(stderr, "krudd_qt: no renderer api\n");
		vulkan_platform_set_host(nullptr);
		subsystem_manager_shutdown(&manager);
		return 1;
	}

	/*
	 * The device is live: boot the render cluster. scene_renderer seeds the
	 * built-in demo scene and records it into the gpu_api each tick. The
	 * Vulkan backend does not yet translate that draw stream (see
	 * renderer_vulkan.c's SCOPE note), so the viewport shows the backend's
	 * animated clear rather than the scene for now; the cluster still boots
	 * so the whole path up to the backend runs. See editor_boot.c for the
	 * registration order and what is deliberately left out.
	 */
	editor_boot_cluster(&manager);

	/*
	 * The camera-aspect half of the wasm viewport bridge, done here since
	 * native has no kruddgui pointer: keep the projection matched to the
	 * viewport's pixel ratio each frame (below), or the scene stretches on
	 * any non-default aspect. Resolved after the cluster, which publishes it.
	 */
	const struct camera_api *camera = static_cast<const struct camera_api *>(
		subsystem_manager_get_api(&manager, "camera"));

	renderer_badge->setText(QStringLiteral("Vulkan · native · ") +
				QGuiApplication::platformName());

	QElapsedTimer fps_clock;
	fps_clock.start();
	int frames_this_second = 0;
	/* Real inter-frame delta feeding the "stats" api, so the scene animates. */
	QElapsedTimer dt_clock;
	dt_clock.start();

	QTimer frame_timer;
	QObject::connect(&frame_timer, &QTimer::timeout, [&]() {
		/* The delta the scene's scripts advance on (ms since last frame). */
		g_stats_api.last_frame_ms = static_cast<float>(dt_clock.restart());

		/* Match the projection to the live viewport so the scene never
		 * stretches; aspect is a ratio, so logical size is enough. */
		if (camera && camera->set_viewport && viewport->height() > 0)
			camera->set_viewport(
				static_cast<float>(viewport->width()),
				static_cast<float>(viewport->height()));

		/*
		 * One frame: the backend pumps its instance, the scene subsystem
		 * runs the entity scripts, and scene_renderer records and executes
		 * the forward pass into the window's backbuffer — the registration
		 * order (renderer, scene, scene_renderer) makes that the tick order.
		 */
		subsystem_manager_tick(&manager);
		gpu->frame_end(); /* acquire, clear and present this frame */

		res_readout->setText(QStringLiteral("%1×%2")
			.arg(viewport->width())
			.arg(viewport->height()));

		frames_this_second++;
		qint64 elapsed = fps_clock.elapsed();
		if (elapsed >= 500) {
			double fps = 1000.0 * frames_this_second / elapsed;
			fps_readout->setText(
				QStringLiteral("fps %1").arg(fps, 0, 'f', 1));
			frames_this_second = 0;
			fps_clock.restart();
		}
	});
	frame_timer.start(16); /* ~60Hz; FIFO present / the compositor set the pace */

	QObject::connect(&app, &QApplication::aboutToQuit, [&]() {
		/* Shut the backend down first (it waits the device idle and
		 * destroys the swapchain and surface), then clear the host — the
		 * host outlives the surface it handed over. */
		subsystem_manager_shutdown(&manager);
		vulkan_platform_set_host(nullptr);
	});

	printf("krudd_qt: presenting on '%s' — close the window or press Esc "
	       "to quit\n",
	       qPrintable(QGuiApplication::platformName()));
	return QApplication::exec();
}
