/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * krudd_qt — the WebGPU backend, presenting into a Qt-hosted native window.
 *
 * The Qt sibling of krudd_window.c (see docs/steamos-window.md and #675):
 * same backend, same pinned Dawn, same webgpu_platform.h seam, same
 * animated-clear proof of life. What changes is who owns the window and who
 * hands Dawn its native surface — here it is a QMainWindow with real editor
 * chrome (menu bar, toolbar, Scene/Inspector docks) around a QWindow
 * embedded via QWidget::createWindowContainer, instead of a bare SDL3
 * top-level window. That embedded QWindow is the "canvas is a window"
 * viewport #675 asks for.
 *
 * Scope is deliberately the same as krudd_window.c's: this does NOT run
 * engine.c's full (Emscripten-only) boot. It drives the backend directly
 * through the gpu_api vtable — an animated clear, redrawn every frame — the
 * same exercise of surface configuration, per-frame acquire, a render pass,
 * submit and present. Rendering the actual scene through this shell is the
 * next step, not this one.
 *
 * DELIBERATELY MOC-FREE. Nothing here declares Q_OBJECT, a signal, or a
 * slot — every connection is to a Qt-supplied QObject (qApp, a QTimer, a
 * QShortcut) through a lambda, which Qt6 allows without a generated moc
 * file. That keeps the kruddmake (qt) clause's job to "add -I/-l flags and
 * compile this .cpp with a C++ compiler" — the same shape as every other
 * native target, with no moc rule to add to ninja.scm. If a future pass
 * needs its own signals, that trade-off is worth revisiting; it is not
 * needed for a proof of life.
 *
 * WAYLAND, THE HARD CASE (see the mockup discussed on #675): getting a
 * QWindow's wl_surface/wl_display out of Qt through *stable public* API
 * needs Qt >= 6.5 (QNativeInterface::QWaylandWindow /
 * QNativeInterface::QWaylandApplication). Verified against a real Qt 6.4.2
 * install (Ubuntu 24.04's system Qt) while writing this: that API is not
 * there, and the only pre-6.5 route is QGuiApplication::platformNativeInterface()
 * reaching into QPlatformNativeInterface, which ships only under
 * qt6-base-private-dev's versioned, ABI-unstable include path. Rather than
 * link the shipped harness against a private header, the Wayland path below
 * requires Qt >= 6.5 and fails the build with an actionable #error otherwise.
 * SteamOS's Arch-based toolchain (docs/steamos-window.md) tracks current Qt,
 * so this is not expected to bite on the actual target hardware — but it is
 * the real, unresolved trade-off #675 asks this work to make explicitly.
 *
 * X11 and Windows do not have that problem: QWindow::winId() is the X11
 * window XID / the Win32 HWND directly, and QNativeInterface::QX11Application
 * (public since Qt 6.0) supplies the X11 Display*. The X11 path below was
 * exercised against a real (Xvfb) X server while writing this file; the
 * Windows path could not be, for lack of the hardware.
 */
extern "C" {
#include "subsystem.h"
#include "subsystem_manager.h"
#include "log.h"
#include "log_api.h"
#include "memory.h"
#include "memory_api.h"
#include "script.h"
#include "renderer.h"        /* struct gpu_api — the backend's vtable */
#include "webgpu_platform.h" /* the native windowing host seam */
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
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
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QWidget>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QtGui/qnativeinterface.h>
#endif

extern "C" {
void renderer_webgpu_plugin_entry(struct subsystem_manager *mgr);
int  renderer_webgpu_device_ready(void);
}

/* Mirrors engine.c's core service table — the same pair krudd_window.c and
 * engine_native.c stand up, static to each Emscripten-only-adjacent TU
 * because it cannot be shared across them. */
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

static const struct subsystem subsystems[] = {
	{ .name = "log",    .api = &g_log_api, .init = log_init, .shutdown = log_shutdown },
	{ .name = "memory", .api = &g_mem_api, .init = mem_init, .shutdown = mem_shutdown },
	{ }
};

static struct subsystem_manager manager;

/* ------------------------------------------------------- the viewport ---- */

/*
 * The embedded native surface. A plain QWindow subclass — no Q_OBJECT, see
 * the file comment — set to VulkanSurface so QPA does not install its own
 * GL-backed backing store over a window an external API (Dawn) presents
 * into. That hint is what SDL_WINDOW_VULKAN communicates on the SDL side of
 * krudd_window.c; Qt has no enumerator for "an external native API owns
 * this," so VulkanSurface is the closest honest answer on every platform
 * Dawn's native backend actually uses (Vulkan on Linux/the Deck; Dawn can
 * also run Vulkan on Windows, which is what this assumes — see the surface
 * note in window_create_surface if that ever needs to become D3D12-aware).
 */
class KruddViewportWindow : public QWindow {
public:
	KruddViewportWindow() { setSurfaceType(QSurface::VulkanSurface); }
};

/*
 * Build the WGPUSurface for the embedded viewport window. Called by the
 * backend through the webgpu_platform host, once, at device bring-up — the
 * Qt sibling of window_create_surface in krudd_window.c, same struct types,
 * same NOTE ON DAWN TYPE NAMES caveat: the WGPUSurfaceSource* structs and
 * their WGPUSType_* tags track Dawn's webgpu.h at the pin
 * (tools/dawn-smoke/README.md). If a Dawn roll renames them, this function
 * and its SDL counterpart are the only two things to adjust.
 */
static WGPUSurface window_create_surface(WGPUInstance instance, void *user)
{
	KruddViewportWindow *win = static_cast<KruddViewportWindow *>(user);
	const QString platform = QGuiApplication::platformName();
	WGPUSurfaceDescriptor sd;

	memset(&sd, 0, sizeof(sd));

#if defined(Q_OS_WIN)
	if (platform == QLatin1String("windows")) {
		WGPUSurfaceSourceWindowsHWND src;

		memset(&src, 0, sizeof(src));
		src.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
		src.hinstance   = GetModuleHandle(nullptr);
		src.hwnd        = reinterpret_cast<HWND>(win->winId());
		sd.nextInChain  = &src.chain;
		return wgpuInstanceCreateSurface(instance, &sd);
	}
#endif

	if (platform == QLatin1String("xcb")) {
		auto *x11app =
			qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
		WGPUSurfaceSourceXlibWindow src;

		if (!x11app || !x11app->display()) {
			fprintf(stderr, "krudd_qt: no X11 display handle\n");
			return nullptr;
		}
		memset(&src, 0, sizeof(src));
		src.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
		src.display     = x11app->display();
		src.window      = static_cast<uint64_t>(win->winId());
		sd.nextInChain  = &src.chain;
		return wgpuInstanceCreateSurface(instance, &sd);
	}

	if (platform == QLatin1String("wayland")) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		auto *wlwin =
			win->nativeInterface<QNativeInterface::QWaylandWindow>();
		auto *wlapp =
			qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
		WGPUSurfaceSourceWaylandSurface src;

		if (!wlwin || !wlapp || !wlwin->surface() || !wlapp->display()) {
			fprintf(stderr, "krudd_qt: no Wayland handles\n");
			return nullptr;
		}
		memset(&src, 0, sizeof(src));
		src.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
		src.display     = wlapp->display();
		src.surface     = wlwin->surface();
		sd.nextInChain  = &src.chain;
		return wgpuInstanceCreateSurface(instance, &sd);
#else
#error "krudd_qt: Wayland needs QNativeInterface::QWaylandWindow, added in " \
       "Qt 6.5. Pre-6.5 Qt has no stable public way to reach a QWindow's " \
       "wl_surface (see the file comment) — build with Qt >= 6.5, or add " \
       "and own a private-header fallback deliberately rather than by " \
       "accident here."
#endif
	}

	fprintf(stderr,
		"krudd_qt: unsupported Qt platform plugin '%s' "
		"(want windows, xcb or wayland)\n",
		qPrintable(platform));
	return nullptr;
}

/* Physical (device) pixels, matching window_drawable_size's SDL counterpart:
 * QWindow::size() is in device-independent pixels, so scale by the window's
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

/* ------------------------------------------------------------- the frame */

/*
 * One frame: clear the backbuffer to an animated colour and present it.
 * Byte-for-byte the same clear krudd_window.c's draw_frame draws — same
 * three sines 120 degrees apart — so the two harnesses are directly
 * comparable proof-of-life artifacts, not just similarly named ones.
 */
static void draw_frame(const struct gpu_api *gpu, uint32_t frame)
{
	struct gpu_render_pass_desc rp;
	gpu_cmd_buf_t cmd;
	float t = static_cast<float>(frame) * 0.02f;

	memset(&rp, 0, sizeof(rp));
	rp.color_count        = 1;
	rp.color[0].texture   = nullptr; /* the backbuffer */
	rp.color[0].load_op   = GPU_LOAD_OP_CLEAR;
	rp.color[0].store_op  = GPU_STORE_OP_STORE;
	rp.color[0].clear[0]  = 0.5f + 0.5f * sinf(t);
	rp.color[0].clear[1]  = 0.5f + 0.5f * sinf(t + 2.094f);
	rp.color[0].clear[2]  = 0.5f + 0.5f * sinf(t + 4.188f);
	rp.color[0].clear[3]  = 1.0f;

	cmd = gpu->cmd_buf_begin();
	gpu->cmd_begin_render_pass(cmd, &rp);
	gpu->cmd_end_render_pass(cmd);
	gpu->cmd_buf_submit(cmd);
	gpu->frame_end(); /* releases the frame's surface texture and presents */
}

/* ------------------------------------------------------------- the shell */

/*
 * Real editor chrome, not just a bare window: a menu bar, a toolbar carrying
 * the renderer badge, and Scene/Inspector docks either side of the embedded
 * viewport. Content is static placeholder — this harness does not run the
 * scene graph (see the file comment) — the point is proving Qt chrome and
 * the native viewport can share one QMainWindow's layout.
 */
static QTreeWidget *build_scene_dock_contents(void)
{
	QTreeWidget *tree = new QTreeWidget();
	QTreeWidgetItem *world;
	QTreeWidgetItem *props;

	tree->setHeaderHidden(true);
	world = new QTreeWidgetItem(tree, QStringList("World"));
	new QTreeWidgetItem(world, QStringList("Camera_Main"));
	props = new QTreeWidgetItem(world, QStringList("Props"));
	new QTreeWidgetItem(props, QStringList("crate_01"));
	tree->expandAll();
	return tree;
}

static QWidget *build_inspector_dock_contents(void)
{
	QLabel *label = new QLabel(
		QStringLiteral("(no live scene — this harness drives the\n"
			       "gpu_api vtable directly, see the file\n"
			       "comment in krudd_qt.cpp)"));
	label->setMargin(8);
	label->setWordWrap(true);
	return label;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setApplicationName(QStringLiteral("krudd"));

	QMainWindow win;
	win.setWindowTitle(QStringLiteral("krudd — Qt editor shell (proof of life)"));

	QMenu *file_menu = win.menuBar()->addMenu(QStringLiteral("&File"));
	file_menu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit,
			     &app, &QApplication::quit);

	QMenu *view_menu = win.menuBar()->addMenu(QStringLiteral("&View"));

	QToolBar *toolbar = win.addToolBar(QStringLiteral("main"));
	QLabel *renderer_badge = new QLabel(QStringLiteral("WebGPU — booting…"));
	toolbar->addWidget(renderer_badge);

	QDockWidget *scene_dock = new QDockWidget(QStringLiteral("Scene"), &win);
	scene_dock->setWidget(build_scene_dock_contents());
	win.addDockWidget(Qt::LeftDockWidgetArea, scene_dock);
	view_menu->addAction(scene_dock->toggleViewAction());

	QDockWidget *inspector_dock =
		new QDockWidget(QStringLiteral("Inspector"), &win);
	inspector_dock->setWidget(build_inspector_dock_contents());
	win.addDockWidget(Qt::RightDockWidgetArea, inspector_dock);
	view_menu->addAction(inspector_dock->toggleViewAction());

	QMenu *help_menu = win.menuBar()->addMenu(QStringLiteral("&Help"));
	help_menu->addAction(QStringLiteral("&About"), [&win]() {
		QMessageBox::information(&win, QStringLiteral("krudd_qt"),
			QStringLiteral(
				"krudd_qt — proof of life for #675.\n\n"
				"The centre panel is a QWindow embedded via "
				"createWindowContainer, presenting into "
				"native Dawn through the same webgpu_platform "
				"seam krudd_window.c uses."));
	});

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

	/*
	 * Register the window host BEFORE the backend boots: renderer_webgpu_init
	 * asks the platform seam for a surface during bring-up, and the host is
	 * what turns that from "offscreen" into "this window" — same ordering
	 * krudd_window.c uses.
	 */
	struct webgpu_platform_host host;

	memset(&host, 0, sizeof(host));
	host.create_surface = window_create_surface;
	host.drawable_size  = window_drawable_size;
	host.user           = viewport;
	webgpu_platform_set_host(&host);

	subsystem_manager_init(&manager, subsystems);
	script_init();
	renderer_webgpu_plugin_entry(&manager);

	/* The adapter/device handshake is async even natively; keep the UI
	 * responsive during the wait (unlike krudd_window.c's tight loop —
	 * Qt already owns an event loop, so there is no reason not to pump
	 * it here too). */
	bool ready = false;
	for (int i = 0; i < 1000 && !ready; i++) {
		subsystem_manager_tick(&manager);
		QApplication::processEvents();
		ready = renderer_webgpu_device_ready();
	}
	if (!ready) {
		fprintf(stderr, "krudd_qt: device never became ready\n");
		webgpu_platform_set_host(nullptr);
		subsystem_manager_shutdown(&manager);
		return 1;
	}

	const struct gpu_api *gpu = static_cast<const struct gpu_api *>(
		subsystem_manager_get_api(&manager, "renderer"));
	if (!gpu) {
		fprintf(stderr, "krudd_qt: no renderer api\n");
		webgpu_platform_set_host(nullptr);
		subsystem_manager_shutdown(&manager);
		return 1;
	}

	renderer_badge->setText(QStringLiteral("WebGPU · Dawn/Vulkan · native · ") +
				QGuiApplication::platformName());

	uint32_t frame = 0;
	QElapsedTimer fps_clock;
	fps_clock.start();
	int frames_this_second = 0;

	QTimer frame_timer;
	QObject::connect(&frame_timer, &QTimer::timeout, [&]() {
		subsystem_manager_tick(&manager); /* pump instance events */
		draw_frame(gpu, frame++);

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
	frame_timer.start(16); /* ~60Hz; Dawn/the compositor set the real pace */

	QObject::connect(&app, &QApplication::aboutToQuit, [&]() {
		/* Clear the host before the backend releases the surface it
		 * created — same ordering krudd_window.c uses at shutdown. */
		webgpu_platform_set_host(nullptr);
		subsystem_manager_shutdown(&manager);
	});

	printf("krudd_qt: presenting on '%s' — close the window or press Esc "
	       "to quit\n",
	       qPrintable(QGuiApplication::platformName()));
	return QApplication::exec();
}
