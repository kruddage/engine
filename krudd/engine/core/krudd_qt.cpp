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
 * The chrome is the authoring surface #676 asks for: a File/Edit/View/Help
 * menu bar like a normal desktop app, and the Scene / Inspector / Assets /
 * Console (Scheme REPL) docks around the viewport. The docks are freely
 * movable, floatable, tabbable and nestable (View > Reset Layout restores the
 * default), so the layout is the user's to rearrange.
 *
 * Scene and Inspector are now wired to the running image (SceneTreePanel /
 * InspectorPanel below), both READ-ONLY: the Scene dock is a live tree of the
 * entity hierarchy that shares the viewport's selection, and the Inspector
 * shows the selected entity's name, parent, components and transform. Assets
 * and Console are still "coming soon" placeholders; write-back editing from the
 * Inspector, the asset browser and the live REPL are the rest of #676.
 *
 * DELIBERATELY MOC-FREE. Nothing here declares Q_OBJECT, a signal, or a
 * slot — every connection is to a Qt-supplied QObject (qApp, a QTimer, a
 * QShortcut) through a lambda, which Qt6 allows without a generated moc
 * file. That keeps the kruddmake (qt) clause's job to "add -I/-l flags and
 * compile this .cpp with a C++ compiler" — the same shape as every other
 * native target, with no moc rule to add to ninja.scm.
 *
 * SURFACE CREATION, PER PLATFORM. The window handles become a VkSurfaceKHR
 * through the matching VK_KHR_*_surface entry point:
 *   - Wayland (the Deck's primary path): QNativeInterface::QWaylandApplication
 *     (public, Qt >= 6.5) gives the wl_display. The per-window wl_surface has
 *     NO public API — QNativeInterface has no QWaylandWindow in any Qt 6
 *     release — so it comes from the QPA native-resource lookup, the one
 *     private header this file leans on. See docs/qt-editor-shell.md.
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
#include "entity_api.h"      /* the "scene" api: clear/build the world (Open) */
#include "asset_api.h"       /* the "asset" api: mesh source for click-to-pick */
#include "editor_boot.h"     /* the native render-cluster boot */
#include "editor_layout.h"   /* the .scm-driven chrome spec walked below (#722) */
#include "version.h"         /* ENGINE_VERSION_STRING — the toolbar badge */
#include "viewport_pick.h"   /* the shared click-to-pick raycast (#697) */
#include "vulkan_platform.h" /* the native windowing host seam (VkSurfaceKHR) */
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QMap>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QShortcut>
#include <QtGui/QWheelEvent>
#include <QtGui/QWindow>
#include <QtGui/qguiapplication_platform.h>
/* The wl_surface behind a QWindow. Qt exposes the *application*-level Wayland
 * handles publicly (QNativeInterface::QWaylandApplication, above) but has no
 * public per-window equivalent — QNativeInterface has no QWaylandWindow, in any
 * Qt 6 release. The QPA native-resource lookup is the only route to it, so this
 * is a deliberate, contained use of one private header: a single "surface"
 * query, whose result is null-checked below like any other handle. */
#include <QtGui/qpa/qplatformnativeinterface.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QTreeWidget>
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

	/*
	 * The engine apis the viewport drives (#697): the scene for the pick
	 * selection, the camera for orbit/pan/dolly, and asset + memory for the
	 * click-to-pick mesh gen. All null until the render cluster boots (set in
	 * main after editor_boot_cluster), so every handler guards them — a press
	 * before the device is up is simply ignored.
	 */
	const struct entity_api *scene  = nullptr;
	const struct camera_api *camera = nullptr;
	const struct asset_api  *asset  = nullptr;
	const struct memory_api *mem    = nullptr;

protected:
	/*
	 * Route the embedded window's pointer + keyboard into the engine. These
	 * are plain virtual overrides — no Q_OBJECT, no slots — so they need no
	 * moc, keeping the (qt) build clause a bare compile of this .cpp (see the
	 * file's MOC-FREE note).
	 *
	 * Left drag orbits, middle/right drag pans, the wheel dollies; a left
	 * click that never turned into a drag picks the entity under the cursor.
	 * Arrow keys nudge the orbit and Home reframes with the scene camera.
	 */
	void mousePressEvent(QMouseEvent *e) override
	{
		m_drag      = e->button();
		m_press_pos = e->position();
		m_last_pos  = e->position();
		m_moved     = false;
		requestActivate(); /* take keyboard focus for the key nav below */
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (m_drag == Qt::NoButton || !camera)
			return;

		QPointF p = e->position();
		QPointF d = p - m_last_pos;
		float   w = width()  > 0 ? (float)width()  : 1.0f;
		float   h = height() > 0 ? (float)height() : 1.0f;

		m_last_pos = p;
		if ((p - m_press_pos).manhattanLength() > 3)
			m_moved = true; /* past this it's a drag, not a click */

		if (m_drag == Qt::LeftButton) {
			/* A full-window drag sweeps about a half turn each axis. */
			if (camera->orbit)
				camera->orbit(-(float)d.x() / w * 3.14159265f,
					      -(float)d.y() / h * 3.14159265f);
		} else if (m_drag == Qt::MiddleButton ||
			   m_drag == Qt::RightButton) {
			if (camera->pan)
				camera->pan((float)d.x() / w, (float)d.y() / h);
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		/* A left press that never became a drag is a pick; a drag was a
		 * camera move and selects nothing. */
		if (m_drag == Qt::LeftButton && !m_moved &&
		    scene && scene->set_selected)
			scene->set_selected(pick_at(e->position()));
		m_drag  = Qt::NoButton;
		m_moved = false;
	}

	void wheelEvent(QWheelEvent *e) override
	{
		if (camera && camera->dolly) {
			/* angleDelta is in eighths of a degree; a notch is 120. */
			float steps = (float)e->angleDelta().y() / 120.0f;

			camera->dolly(steps * 0.1f);
			e->accept();
		}
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		if (!camera) {
			QWindow::keyPressEvent(e);
			return;
		}
		switch (e->key()) {
		case Qt::Key_Left:
			if (camera->orbit) camera->orbit(-0.1f, 0.0f);
			break;
		case Qt::Key_Right:
			if (camera->orbit) camera->orbit(0.1f, 0.0f);
			break;
		case Qt::Key_Up:
			if (camera->orbit) camera->orbit(0.0f, 0.1f);
			break;
		case Qt::Key_Down:
			if (camera->orbit) camera->orbit(0.0f, -0.1f);
			break;
		case Qt::Key_Home:
			if (camera->reset_view) camera->reset_view();
			break;
		default:
			QWindow::keyPressEvent(e);
		}
	}

private:
	/* Pick the entity under a window-relative pixel, in the same logical-pixel
	 * space as the per-frame aspect sync, so the ray matches what was drawn. */
	int32_t pick_at(const QPointF &pos)
	{
		struct mat4 vp;
		const struct world *w;

		if (!scene || !camera || !asset || !mem)
			return -1;
		w = scene->get_world();
		if (!w)
			return -1;
		camera->get_view_proj(&vp);
		return viewport_pick_entity(w, &vp, (float)pos.x(),
					    (float)pos.y(),
					    width() > 0 ? (float)width() : 1.0f,
					    height() > 0 ? (float)height() : 1.0f,
					    asset, mem);
	}

	QPointF         m_press_pos;
	QPointF         m_last_pos;
	Qt::MouseButton m_drag  = Qt::NoButton;
	bool            m_moved = false;
};

/*
 * The instance extensions window_create_surface below needs. Handed to the
 * backend through the platform host, and enabled at vkCreateInstance — without
 * them the matching VkCreate*SurfaceKHR fails VK_ERROR_EXTENSION_NOT_PRESENT.
 * renderer_vulkan.c must not know which windowing system we are on, so this
 * file — the one that already does — declares it, and every entry here has a
 * matching branch in window_create_surface.
 *
 * Listing both Wayland and XCB on Linux is deliberate: which one is live is a
 * runtime property of the Qt platform plugin, and the backend already drops
 * whatever the loader does not report, so naming both costs nothing and keeps
 * this list a compile-time constant.
 */
static const char *const k_instance_extensions[] = {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
	VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
	VK_KHR_XCB_SURFACE_EXTENSION_NAME,
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
	VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
#endif
	VK_KHR_SURFACE_EXTENSION_NAME,
};

/*
 * Build the VkSurfaceKHR for the embedded viewport window. Called by the
 * backend through the vulkan_platform host, once, at device bring-up. The
 * per-platform branches mirror the VK_USE_PLATFORM_* selection at the top of
 * the file; if a future platform is added, that selection, the extension list
 * above and this function are the three places to touch.
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
		VkResult r = vkCreateWin32SurfaceKHR(instance, &ci, nullptr,
						     &surface);
		if (r != VK_SUCCESS) {
			fprintf(stderr,
				"krudd_qt: vkCreateWin32SurfaceKHR failed (%d)\n",
				(int)r);
			return VK_NULL_HANDLE;
		}
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
		VkResult r = vkCreateXcbSurfaceKHR(instance, &ci, nullptr,
						   &surface);
		if (r != VK_SUCCESS) {
			fprintf(stderr,
				"krudd_qt: vkCreateXcbSurfaceKHR failed (%d)\n",
				(int)r);
			return VK_NULL_HANDLE;
		}
		return surface;
	}
#endif

#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
	if (platform == QLatin1String("wayland")) {
		auto *wlapp =
			qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
		QPlatformNativeInterface *pni = qGuiApp->platformNativeInterface();
		VkWaylandSurfaceCreateInfoKHR ci;
		struct wl_surface *wlsurf = nullptr;

		if (pni)
			wlsurf = static_cast<struct wl_surface *>(
				pni->nativeResourceForWindow("surface", win));
		if (!wlapp || !wlsurf || !wlapp->display()) {
			fprintf(stderr, "krudd_qt: no Wayland handles\n");
			return VK_NULL_HANDLE;
		}
		memset(&ci, 0, sizeof(ci));
		ci.sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
		ci.display = wlapp->display();
		ci.surface = wlsurf;
		VkResult r = vkCreateWaylandSurfaceKHR(instance, &ci, nullptr,
						       &surface);
		if (r != VK_SUCCESS) {
			fprintf(stderr,
				"krudd_qt: vkCreateWaylandSurfaceKHR failed (%d)\n",
				(int)r);
			return VK_NULL_HANDLE;
		}
		return surface;
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
 * docks around the embedded viewport. Scene and Inspector carry live,
 * read-only bodies wired to the running image (see the live-panels section
 * above); Assets and Console are still "coming soon" placeholders.
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

/* ------------------------------------------------------- the live panels ---- */

/*
 * Two of the docks are wired to the running image (#676): Scene shows the live
 * entity hierarchy and Inspector shows the selected entity's properties. Both
 * are READ-ONLY for now — they render the world, they don't yet write it back
 * (transform/name editing is the follow-up). Assets and Console stay
 * make_placeholder "coming soon" bodies.
 *
 * Both panels are driven by polling the shared "scene" api from the frame loop
 * (SceneTreePanel::refresh / InspectorPanel::refresh, called each tick) rather
 * than by any push signal — there is none — so they stay MOC-FREE: neither
 * declares Q_OBJECT, and the one signal we do use (a QTreeWidget's built-in
 * itemSelectionChanged) is connected to a lambda, exactly as every QAction here
 * is. The "scene" api is published only once editor_boot_cluster runs, so the
 * host calls set_scene() on each panel after boot; until then refresh() is a
 * guarded no-op.
 */

/* A cheap change-signature over the world's *structure and naming* — count,
 * name bytes, and each live slot's alive/parent/mask/name offset. It deliberately
 * ignores transforms, so an animating scene does not rebuild the tree every
 * frame; the selection is synced separately and cheaply regardless. */
static uint64_t scene_tree_signature(const struct world *w)
{
	uint64_t h = 1469598103934665603ULL; /* FNV-1a offset basis */
	auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ULL; };

	mix(w->count);
	mix(w->name_bytes);
	for (uint32_t i = 0; i < w->count; i++) {
		mix(w->alive[i]);
		mix(static_cast<uint32_t>(w->parent[i]));
		mix(w->mask[i]);
		mix(w->name_off[i]);
	}
	return h;
}

/*
 * The Scene panel: a QTreeWidget mirroring the world's parent[] hierarchy, with
 * two-way selection sync against the shared "scene" model. Clicking a node
 * drives set_selected; a viewport pick (or any other selection change) is
 * reflected back into the tree each frame. Rebuilt only when the structure
 * signature changes, so it is cheap to poll at 60Hz.
 */
class SceneTreePanel {
public:
	SceneTreePanel()
	{
		m_tree = new QTreeWidget();
		m_tree->setHeaderHidden(true);
		m_tree->setColumnCount(1);
		m_tree->setSelectionMode(QAbstractItemView::SingleSelection);

		/* User picked a node -> drive the shared selection. Guarded so the
		 * programmatic sync in refresh() below does not loop back here as if
		 * it were a user edit. */
		QObject::connect(
			m_tree, &QTreeWidget::itemSelectionChanged, [this]() {
				if (m_syncing || !m_scene || !m_scene->set_selected)
					return;
				QList<QTreeWidgetItem *> sel =
					m_tree->selectedItems();
				int32_t id = sel.isEmpty()
						? -1
						: sel.first()->data(0, Qt::UserRole)
							  .toInt();
				m_scene->set_selected(id);
			});
	}

	QWidget *widget() { return m_tree; }
	void set_scene(const struct entity_api *s) { m_scene = s; }

	void refresh()
	{
		if (!m_scene || !m_scene->get_world)
			return;
		const struct world *w = m_scene->get_world();
		if (!w)
			return;

		uint64_t sig = scene_tree_signature(w);
		if (!m_built || sig != m_sig) {
			rebuild(w);
			m_sig   = sig;
			m_built = true;
		}
		sync_selection(w);
	}

private:
	/* Rebuild the whole tree from the world. Entities are stored parent-
	 * before-child, so a parent's item always exists by the time its children
	 * are placed; a live entity never has a tombstoned parent (destroy
	 * cascades), but we fall back to the root if a parent item is somehow
	 * missing rather than drop the node. */
	void rebuild(const struct world *w)
	{
		m_syncing = true; /* clear()/addItem churn must not fire set_selected */
		m_tree->clear();
		m_items.clear();

		for (uint32_t i = 0; i < w->count; i++) {
			if (!w->alive[i])
				continue;

			const char *name  = world_entity_name(w, i);
			QString     label = (name && *name)
						    ? QString::fromUtf8(name)
						    : QStringLiteral("Entity %1").arg(i);
			QTreeWidgetItem *item = new QTreeWidgetItem();
			item->setText(0, label);
			item->setData(0, Qt::UserRole, static_cast<int>(i));

			int32_t          parent = w->parent[i];
			QTreeWidgetItem *pitem  = (parent >= 0)
							 ? m_items.value(parent, nullptr)
							 : nullptr;
			if (pitem)
				pitem->addChild(item);
			else
				m_tree->addTopLevelItem(item);
			m_items.insert(static_cast<int32_t>(i), item);
		}
		m_tree->expandAll();
		m_syncing = false;
	}

	/* Reflect the shared selection into the tree, without re-emitting it. */
	void sync_selection(const struct world *w)
	{
		int32_t sel = m_scene->get_selected ? m_scene->get_selected()
						    : w->selected;
		QTreeWidgetItem *want =
			(sel >= 0) ? m_items.value(sel, nullptr) : nullptr;
		QList<QTreeWidgetItem *> have = m_tree->selectedItems();
		QTreeWidgetItem *cur = have.isEmpty() ? nullptr : have.first();
		if (cur == want)
			return;

		m_syncing = true;
		if (want) {
			m_tree->setCurrentItem(want);
			m_tree->scrollToItem(want);
		} else {
			m_tree->clearSelection();
			m_tree->setCurrentItem(nullptr);
		}
		m_syncing = false;
	}

	QTreeWidget                      *m_tree  = nullptr;
	const struct entity_api          *m_scene = nullptr;
	QHash<int32_t, QTreeWidgetItem *> m_items;
	uint64_t                          m_sig     = 0;
	bool                              m_built   = false;
	bool                              m_syncing = false;
};

/* The set component bits of an entity, as a readable " · "-joined list. */
static QString component_summary(uint32_t mask)
{
	QStringList c;

	if (mask & COMPONENT_NAME)     c << QStringLiteral("Name");
	if (mask & COMPONENT_RENDER)   c << QStringLiteral("Render");
	if (mask & COMPONENT_MATERIAL) c << QStringLiteral("Material");
	if (mask & COMPONENT_SCRIPT)   c << QStringLiteral("Script");
	if (mask & COMPONENT_LIGHT)    c << QStringLiteral("Light");
	return c.isEmpty() ? QStringLiteral("—")
			   : c.join(QStringLiteral(" · "));
}

/*
 * The Inspector panel: a read-only properties view of the selected entity —
 * name, id, parent, components and the authored (local) transform. A single
 * rich-text QLabel, refreshed every frame so a scripted entity's transform
 * animates live; when nothing is selected it shows a short hint. Editing the
 * fields back through set_transform/set_name is the follow-up pass.
 */
class InspectorPanel {
public:
	InspectorPanel()
	{
		m_label = new QLabel();
		m_label->setTextFormat(Qt::RichText);
		m_label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
		m_label->setWordWrap(true);
		m_label->setMargin(12);
		m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
		show_empty();
	}

	QWidget *widget() { return m_label; }
	void set_scene(const struct entity_api *s) { m_scene = s; }

	void refresh()
	{
		const struct world *w = (m_scene && m_scene->get_world)
						? m_scene->get_world()
						: nullptr;
		int32_t sel = -1;
		if (m_scene && m_scene->get_selected)
			sel = m_scene->get_selected();
		else if (w)
			sel = w->selected;

		if (!w || sel < 0 || static_cast<uint32_t>(sel) >= w->count ||
		    !w->alive[sel]) {
			show_empty();
			return;
		}
		/* Repaint every frame: a scripted entity's transform changes under a
		 * fixed selection, so gating on "selection changed" would freeze the
		 * numbers. The cost is a handful of setText's — cheap. */
		m_label->setText(describe(w, static_cast<uint32_t>(sel)));
		m_shown = sel;
	}

private:
	void show_empty()
	{
		if (m_shown == -1)
			return; /* already showing the empty state */
		m_label->setText(QStringLiteral(
			"<p style='color:gray'>No entity selected.</p>"
			"<p style='color:gray'>Pick a node in the Scene panel, "
			"or click an object in the viewport.</p>"));
		m_shown = -1;
	}

	static QString vec3(const float *p)
	{
		return QStringLiteral("%1, %2, %3")
			.arg(p[0], 0, 'f', 3)
			.arg(p[1], 0, 'f', 3)
			.arg(p[2], 0, 'f', 3);
	}

	static QString quat(const float *p)
	{
		return QStringLiteral("%1, %2, %3, %4")
			.arg(p[0], 0, 'f', 3)
			.arg(p[1], 0, 'f', 3)
			.arg(p[2], 0, 'f', 3)
			.arg(p[3], 0, 'f', 3);
	}

	static QString describe(const struct world *w, uint32_t e)
	{
		const char *name = world_entity_name(w, e);
		QString     nm   = (name && *name)
					 ? QString::fromUtf8(name).toHtmlEscaped()
					 : QStringLiteral("<i>(unnamed)</i>");
		const struct transform *t = &w->local[e];

		QString parent;
		if (w->parent[e] < 0) {
			parent = QStringLiteral("— <span style='color:gray'>"
						"(root)</span>");
		} else {
			const char *pn = world_entity_name(
				w, static_cast<uint32_t>(w->parent[e]));
			parent = (pn && *pn)
					 ? QStringLiteral("%1 <span style='color:gray'>"
							  "(#%2)</span>")
						   .arg(QString::fromUtf8(pn)
								.toHtmlEscaped())
						   .arg(w->parent[e])
					 : QStringLiteral("#%1").arg(w->parent[e]);
		}

		auto row = [](const QString &k, const QString &v) {
			return QStringLiteral(
				       "<tr><td style='color:gray;"
				       "padding-right:10px'>%1</td>"
				       "<td>%2</td></tr>")
				.arg(k, v);
		};

		return QStringLiteral("<p style='font-size:14px'><b>%1</b></p>")
			       .arg(nm) +
		       QStringLiteral("<table cellspacing='2'>") +
		       row(QStringLiteral("Id"), QStringLiteral("#%1").arg(e)) +
		       row(QStringLiteral("Parent"), parent) +
		       row(QStringLiteral("Components"),
			   component_summary(w->mask[e])) +
		       row(QStringLiteral("Position"), vec3(t->position)) +
		       row(QStringLiteral("Rotation"), quat(t->rotation) +
				   QStringLiteral(" <span style='color:gray'>"
						  "(xyzw)</span>")) +
		       row(QStringLiteral("Scale"), vec3(t->scale)) +
		       QStringLiteral("</table>");
	}

	QLabel                  *m_label = nullptr;
	const struct entity_api *m_scene = nullptr;
	int32_t                  m_shown = -2; /* -2 forces the first paint */
};

/* The live panels the boot path hands the "scene" api and the frame loop
 * refreshes. Null when the spec declares no such dock. */
struct ChromePanels {
	SceneTreePanel *scene_tree = nullptr;
	InspectorPanel *inspector  = nullptr;
};

/* ----------------------------------------------- the .scm-driven chrome ---- */

/*
 * The editor chrome is authored as one Scheme data spec (core/editor_layout.scm,
 * embedded as LAYOUT_SCM) and walked into a plain C `struct editor_layout` by
 * editor_layout.c (#722). build_editor_chrome below emits the equivalent Qt
 * widgets from that struct, so nothing here hard-codes a menu, dock, toolbar or
 * status literal — the spec is the single source of the shell's structure.
 *
 * A handful of action ids the spec attaches to entries are wired to real
 * behavior (Open Project, Quit, About, Reset Layout); every other id falls
 * through to the "coming soon" status hint. The host passes those behaviors in
 * as callbacks so this walker stays free of engine specifics.
 */

/* The spec's standard-key symbol, mapped to the platform's QKeySequence so the
 * shortcuts stay native (Ctrl+O / ⌘O …) exactly as the hand-written menus did. */
static QKeySequence shortcut_for(enum editor_shortcut s)
{
	switch (s) {
	case EDITOR_KEY_NEW:     return QKeySequence(QKeySequence::New);
	case EDITOR_KEY_OPEN:    return QKeySequence(QKeySequence::Open);
	case EDITOR_KEY_SAVE:    return QKeySequence(QKeySequence::Save);
	case EDITOR_KEY_SAVE_AS: return QKeySequence(QKeySequence::SaveAs);
	case EDITOR_KEY_QUIT:    return QKeySequence(QKeySequence::Quit);
	case EDITOR_KEY_UNDO:    return QKeySequence(QKeySequence::Undo);
	case EDITOR_KEY_REDO:    return QKeySequence(QKeySequence::Redo);
	case EDITOR_KEY_CUT:     return QKeySequence(QKeySequence::Cut);
	case EDITOR_KEY_COPY:    return QKeySequence(QKeySequence::Copy);
	case EDITOR_KEY_PASTE:   return QKeySequence(QKeySequence::Paste);
	case EDITOR_KEY_NONE:
	default:                 return QKeySequence();
	}
}

static Qt::DockWidgetArea area_for(enum editor_dock_area a)
{
	switch (a) {
	case EDITOR_AREA_RIGHT:  return Qt::RightDockWidgetArea;
	case EDITOR_AREA_TOP:    return Qt::TopDockWidgetArea;
	case EDITOR_AREA_BOTTOM: return Qt::BottomDockWidgetArea;
	case EDITOR_AREA_LEFT:
	default:                 return Qt::LeftDockWidgetArea;
	}
}

/* The "coming soon" hint text for an unwired entry: the menu/toolbar label with
 * its accelerator '&', a trailing '…', and any leading glyph ("▶ ", "■ ")
 * stripped, so "Save &As…" reads "Save As" and "▶ Play" reads "Play" — the same
 * wording the hand-written status hints used. */
static QString hint_label(const QString &raw)
{
	QString s = raw;

	s.remove(QLatin1Char('&'));
	s.remove(QChar(0x2026)); /* … */
	int i = 0;
	while (i < s.size() && !s.at(i).isLetterOrNumber())
		i++;
	return s.mid(i).trimmed();
}

/* The behaviors the spec's wired action ids resolve to, supplied by the host so
 * the walker below stays engine-agnostic. Everything else is a coming_soon. */
struct ChromeActions {
	std::function<void(const QString &)> coming_soon;
	std::function<void()>                open_project;
	std::function<void()>                quit;
	std::function<void()>                about;
	std::function<void()>                reset_layout;
};

/* The live widgets the frame loop and boot path update after the walk: the
 * status fields (fps / resolution / driver) and toolbar badges (renderer),
 * keyed by their spec ids. */
struct EditorChrome {
	QMap<QString, QLabel *> status;
	QMap<QString, QLabel *> badges;
};

/* Wire one menu action's id to its behavior: the few wired ids to their host
 * callbacks, every other id to the coming_soon hint derived from its label. */
static void connect_action(QAction *a, const QString &id, const QString &label,
			   const ChromeActions &act)
{
	if (id == QLatin1String("open-project"))
		QObject::connect(a, &QAction::triggered, act.open_project);
	else if (id == QLatin1String("quit"))
		QObject::connect(a, &QAction::triggered, act.quit);
	else if (id == QLatin1String("about"))
		QObject::connect(a, &QAction::triggered, act.about);
	else if (id == QLatin1String("reset-layout"))
		QObject::connect(a, &QAction::triggered, act.reset_layout);
	else {
		auto soon = act.coming_soon;
		QObject::connect(a, &QAction::triggered,
				 [soon, label]() { soon(label); });
	}
}

/*
 * Emit the whole editor chrome from the evaluated layout spec. Docks come first
 * so the View menu's (dock-toggles) can reference their toggle actions; then the
 * menus, the toolbar and the status bar. Returns the handles the caller updates
 * live. The QMainWindow objectNames (window, toolbar) and dock objectNames come
 * from the spec, so saveState/restoreState and View ▸ Reset Layout round-trip.
 */
static EditorChrome build_editor_chrome(QMainWindow &win,
					const struct editor_layout *L,
					const ChromeActions &act,
					ChromePanels &panels)
{
	EditorChrome                    chrome;
	QMap<QString, QDockWidget *>    docks_by_id;
	QList<QDockWidget *>            docks_in_order;

	/* ---- docks (build before menus so dock-toggles can resolve) ------ */
	for (uint32_t i = 0; i < L->dock_count; i++) {
		const struct editor_dock *d  = &L->docks[i];
		QString                   id = QString::fromUtf8(d->id);

		/* Two dock ids get a live body wired to the running image; every
		 * other falls through to the "coming soon" placeholder — the same
		 * known-id dispatch connect_action uses for menu actions. */
		QWidget *body;
		if (id == QLatin1String("dock.scene")) {
			panels.scene_tree = new SceneTreePanel();
			body              = panels.scene_tree->widget();
		} else if (id == QLatin1String("dock.inspector")) {
			panels.inspector = new InspectorPanel();
			body             = panels.inspector->widget();
		} else {
			body = make_placeholder(QString::fromUtf8(d->panel),
						QString::fromUtf8(d->blurb));
		}

		QDockWidget *dock =
			make_dock(QString::fromUtf8(d->title), id, body, &win);

		win.addDockWidget(area_for(d->area), dock);
		docks_by_id.insert(id, dock);
		docks_in_order.append(dock);
	}
	/* Tab grouping is a second pass: a (tabbed-with id) can name a dock
	 * declared after it, so every dock must exist before tabifying. */
	for (uint32_t i = 0; i < L->dock_count; i++) {
		const struct editor_dock *d = &L->docks[i];

		if (d->tabbed_with[0]) {
			QDockWidget *self =
				docks_by_id.value(QString::fromUtf8(d->id));
			QDockWidget *onto = docks_by_id.value(
				QString::fromUtf8(d->tabbed_with));
			if (self && onto)
				win.tabifyDockWidget(onto, self);
		}
	}
	/* Raise last, so the tab that wins its group is shown on top. */
	for (uint32_t i = 0; i < L->dock_count; i++) {
		const struct editor_dock *d = &L->docks[i];

		if (d->raise) {
			QDockWidget *self =
				docks_by_id.value(QString::fromUtf8(d->id));
			if (self)
				self->raise();
		}
	}

	/* ---- menus ------------------------------------------------------- */
	for (uint32_t i = 0; i < L->menu_count; i++) {
		const struct editor_menu *m = &L->menus[i];
		QMenu *menu =
			win.menuBar()->addMenu(QString::fromUtf8(m->label));

		for (uint32_t j = 0; j < m->item_count; j++) {
			const struct editor_menu_item *it = &m->items[j];

			switch (it->kind) {
			case EDITOR_ITEM_SEPARATOR:
				menu->addSeparator();
				break;
			case EDITOR_ITEM_DOCK_TOGGLES:
				for (QDockWidget *d : docks_in_order)
					menu->addAction(d->toggleViewAction());
				break;
			case EDITOR_ITEM_ACTION: {
				QAction *a = menu->addAction(
					QString::fromUtf8(it->label));
				QKeySequence ks = shortcut_for(it->shortcut);

				if (!ks.isEmpty())
					a->setShortcut(ks);
				connect_action(
					a, QString::fromUtf8(it->action),
					hint_label(QString::fromUtf8(it->label)),
					act);
				break;
			}
			}
		}
	}

	/* ---- toolbar ----------------------------------------------------- */
	QToolBar *toolbar = win.addToolBar(QStringLiteral("main"));
	toolbar->setObjectName(QStringLiteral("toolbar.main"));
	for (uint32_t i = 0; i < L->tool_count; i++) {
		const struct editor_tool *t = &L->tools[i];

		switch (t->kind) {
		case EDITOR_TOOL_SEPARATOR:
			toolbar->addSeparator();
			break;
		case EDITOR_TOOL_ITEM: {
			QAction *a = toolbar->addAction(
				QString::fromUtf8(t->label));
			QString  label = hint_label(QString::fromUtf8(t->label));
			auto     soon  = act.coming_soon;

			QObject::connect(a, &QAction::triggered,
					 [soon, label]() { soon(label); });
			break;
		}
		case EDITOR_TOOL_BADGE: {
			QLabel *badge = new QLabel(QString::fromUtf8(t->label));

			toolbar->addWidget(badge);
			chrome.badges.insert(QString::fromUtf8(t->id), badge);
			break;
		}
		case EDITOR_TOOL_SPACER: {
			/* An empty widget that claims every spare pixel, which
			 * is how a QToolBar right-aligns: it has no alignment
			 * of its own, so the gap does the pushing. */
			QWidget *gap = new QWidget;

			gap->setSizePolicy(QSizePolicy::Expanding,
					   QSizePolicy::Preferred);
			toolbar->addWidget(gap);
			break;
		}
		}
	}

	/* ---- status bar -------------------------------------------------- */
	for (uint32_t i = 0; i < L->status_count; i++) {
		const struct editor_status_field *f = &L->status[i];
		QLabel *lbl = new QLabel(QString::fromUtf8(f->text));

		win.statusBar()->addPermanentWidget(lbl);
		chrome.status.insert(QString::fromUtf8(f->id), lbl);
	}

	return chrome;
}

/*
 * Load a .scm scene off disk into the live world (#698) — the native File ▸
 * Open Project path, the counterpart of the browser's IndexedDB/fetch scene
 * sources that editor_boot_cluster deliberately leaves out. clear_world drops
 * the current scene first so Open replaces rather than layers, build_scene_scm
 * evaluates the authored (scene …) form against the shared s7 image (the same
 * entry a built-in game boots its world through), and reset_view hands the
 * camera back to the freshly loaded scene's scripted camera. Returns the
 * spawned entity count, or -1 with *err set on any failure.
 */
static int load_scene_file(const struct entity_api *scene,
			   const struct camera_api *camera,
			   const QString &path, QString *err)
{
	if (!scene || !scene->build_scene_scm) {
		*err = QStringLiteral("scene subsystem is not available");
		return -1;
	}

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		*err = QStringLiteral("cannot open %1: %2")
			       .arg(path, f.errorString());
		return -1;
	}
	QByteArray src = f.readAll();
	f.close();

	/* Replace, don't layer: empty the world before building the new scene. */
	if (scene->clear_world)
		scene->clear_world();

	int n = scene->build_scene_scm(src.constData());
	if (n < 0) {
		*err = QStringLiteral("%1 is not a valid (scene …) form")
			       .arg(QFileInfo(path).fileName());
		return -1;
	}

	/* Frame the loaded scene with its own camera, dropping any prior view. */
	if (camera && camera->reset_view)
		camera->reset_view();
	return n;
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

	/* Panels are placeholders for now (#676), so every menu/toolbar action
	 * that is not wired yet says so in the status bar rather than doing
	 * nothing — the honest desktop-app hint, without a modal on every click. */
	auto coming_soon = [&win](const QString &what) {
		win.statusBar()->showMessage(
			what + QStringLiteral(" — coming soon"), 4000);
	};

	/*
	 * The scene ("scene") and camera apis the wired menu actions drive. Both
	 * are published by the render cluster, so they stay null until
	 * editor_boot_cluster runs (far below); the Open Project handler resolves
	 * them at click time — long after boot — through these, captured by
	 * reference. A build that never boots the cluster leaves them null and the
	 * handlers degrade to a status-bar note rather than dereferencing null.
	 */
	const struct entity_api *scene  = nullptr;
	const struct camera_api *camera = nullptr;

	/* The default layout snapshot, filled once the window is shown (below).
	 * View ▸ Reset Layout restores it, so the reset callback captures it by
	 * reference — the dock objectNames it round-trips come from the spec. */
	QByteArray default_geometry;
	QByteArray default_state;

	/*
	 * The chrome — menu bar, toolbar, docks, status bar — is authored as one
	 * Scheme data spec (core/editor_layout.scm) and walked into a C tree by
	 * editor_layout.c (#722); build_editor_chrome emits the Qt widgets from
	 * it, so no menu/dock/toolbar literal is hard-coded here. Evaluate the
	 * spec live through the shared s7 image, the same way script.c loads its
	 * other DSL images. A spec that fails to evaluate is a build breakage, so
	 * fail loudly rather than opening an empty shell.
	 */
	struct editor_layout layout;
	if (editor_layout_load(&layout) != 0) {
		fprintf(stderr,
			"krudd_qt: could not load the editor layout spec\n");
		return 1;
	}

	/*
	 * The behaviors the spec's wired action ids resolve to. Everything else
	 * the walker connects to the coming_soon hint. open_project/reset_layout
	 * capture scene/camera and the layout snapshot by reference, so they act
	 * on the live values at click time — long after this returns.
	 */
	ChromeActions actions;
	actions.coming_soon  = coming_soon;
	actions.quit         = [&app]() { app.quit(); };
	actions.reset_layout = [&]() {
		if (!default_geometry.isEmpty())
			win.restoreGeometry(default_geometry);
		if (!default_state.isEmpty())
			win.restoreState(default_state);
	};
	actions.about = [&win]() {
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
				"default.<br><br>"
				"Scene and Inspector are live (read-only): "
				"the Scene tree mirrors the entity hierarchy "
				"and shares the viewport's selection, and the "
				"Inspector shows the selected entity's "
				"properties."));
	};
	actions.open_project = [&]() {
		const QString path = QFileDialog::getOpenFileName(
			&win, QStringLiteral("Open Project"), QString(),
			QStringLiteral("Scene scripts (*.scm);;"
				       "All files (*)"));

		if (path.isEmpty())
			return; /* user cancelled the dialog */

		QString err;
		int n = load_scene_file(scene, camera, path, &err);
		if (n < 0) {
			win.statusBar()->showMessage(err, 6000);
			QMessageBox::warning(
				&win, QStringLiteral("Open Project"), err);
			return;
		}
		win.statusBar()->showMessage(
			QStringLiteral("Loaded %1 — %2 entit%3")
				.arg(QFileInfo(path).fileName())
				.arg(n)
				.arg(n == 1 ? QStringLiteral("y")
					    : QStringLiteral("ies")),
			6000);
	};

	/* The live panels (Scene tree, Inspector) the walker wires to real
	 * bodies; handed the "scene" api after the cluster boots, refreshed each
	 * frame. Null for any dock the spec does not declare. */
	ChromePanels panels;

	EditorChrome chrome = build_editor_chrome(win, &layout, actions, panels);

	KruddViewportWindow *viewport = new KruddViewportWindow();
	QWidget *viewport_container =
		QWidget::createWindowContainer(viewport, &win);
	viewport_container->setMinimumSize(320, 240);
	win.setCentralWidget(viewport_container);

	/* The live widgets the boot path and frame loop update, resolved by their
	 * spec ids. The driver field shows the Qt platform plugin (xcb/wayland/
	 * windows), set once here — the spec seeds it empty. */
	QLabel *renderer_badge = chrome.badges.value(QStringLiteral("renderer"));
	QLabel *fps_readout    = chrome.status.value(QStringLiteral("fps"));
	QLabel *res_readout    = chrome.status.value(QStringLiteral("resolution"));
	/* The version badge is fixed for the life of the process, so it is
	 * finished here rather than in the frame loop — the spec seeds the
	 * product name, this appends the semver the build was stamped with. */
	if (QLabel *version_badge =
		    chrome.badges.value(QStringLiteral("version")))
		version_badge->setText(version_badge->text() +
				       QStringLiteral(" " ENGINE_VERSION_STRING));
	if (QLabel *driver_readout =
		    chrome.status.value(QStringLiteral("driver")))
		driver_readout->setText(
			QGuiApplication::platformName().toUpper());

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
	 * reads our instance extensions while creating the instance and asks for
	 * a surface during bring-up, and the host is what turns that from
	 * "headless" into "this window". Registering it after either point
	 * silently costs presentation, so this must stay ahead of
	 * subsystem_manager_init below.
	 */
	struct vulkan_platform_host host;

	memset(&host, 0, sizeof(host));
	host.create_surface           = window_create_surface;
	host.drawable_size            = window_drawable_size;
	host.instance_extensions      = k_instance_extensions;
	host.instance_extension_count =
		sizeof(k_instance_extensions) / sizeof(k_instance_extensions[0]);
	host.user                     = viewport;
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
	 * Resolve the cluster's apis now it is up: the camera drives the
	 * per-frame aspect sync (below) and the scene backs File ▸ Open Project.
	 * The camera-aspect sync is the half of the wasm viewport bridge done
	 * here, since native has no kruddgui pointer — keep the projection matched
	 * to the viewport each frame or the scene stretches on any non-default
	 * aspect. Both fill the pointers the menu handlers captured by reference.
	 */
	camera = static_cast<const struct camera_api *>(
		subsystem_manager_get_api(&manager, "camera"));
	scene = static_cast<const struct entity_api *>(
		subsystem_manager_get_api(&manager, "scene"));

	/*
	 * Hand the viewport its apis so its pointer/key handlers can pick and
	 * navigate (#697): the scene for the selection, the camera for orbit/pan/
	 * dolly, and asset + memory for the click-to-pick mesh gen. Set after the
	 * cluster so they are live before the first event; the memory api is the
	 * file's own static table, the same one the subsystems run on.
	 */
	viewport->scene  = scene;
	viewport->camera = camera;
	viewport->asset  = static_cast<const struct asset_api *>(
		subsystem_manager_get_api(&manager, "asset"));
	viewport->mem    = &g_mem_api;

	/* Hand the live panels the same "scene" api so they can read the world
	 * and share its selection model; until now their refresh() was a guarded
	 * no-op (the api did not exist before the cluster booted). */
	if (panels.scene_tree)
		panels.scene_tree->set_scene(scene);
	if (panels.inspector)
		panels.inspector->set_scene(scene);

	if (renderer_badge)
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

		/* Reflect this frame's world into the live docks: the Scene tree
		 * (structure + selection) and the Inspector (selected entity's
		 * properties, which animate under a scripted entity). Both poll the
		 * shared "scene" api and no-op cheaply when nothing changed. */
		if (panels.scene_tree)
			panels.scene_tree->refresh();
		if (panels.inspector)
			panels.inspector->refresh();

		if (res_readout)
			res_readout->setText(QStringLiteral("%1×%2")
				.arg(viewport->width())
				.arg(viewport->height()));

		frames_this_second++;
		qint64 elapsed = fps_clock.elapsed();
		if (elapsed >= 500) {
			double fps = 1000.0 * frames_this_second / elapsed;
			if (fps_readout)
				fps_readout->setText(
					QStringLiteral("fps %1")
						.arg(fps, 0, 'f', 1));
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
