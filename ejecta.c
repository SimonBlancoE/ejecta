/*
 * Ejecta — removable drives in the system tray.
 *
 * A StatusNotifierItem (works with any SNI tray: Noctalia, Waybar, KDE…) with:
 *   - left click: a floating panel (layer-shell on Wayland) listing drives,
 *     their details and the basic actions;
 *   - right click: a full menu (dbusmenu) with every action and the settings;
 *   - notifications when a device is plugged in (ask to mount) and, optionally,
 *     when it gets mounted (ask to open the file manager);
 *   - when a drive can't be unmounted because it is busy, it names the
 *     processes holding it and can close them and retry.
 *
 * Everything talks to UDisks2 over D-Bus directly: no helper processes.
 *
 * SPDX-License-Identifier: MIT
 */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <locale.h>
#include <sys/statvfs.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#ifndef VERSION
#define VERSION "dev"
#endif
#ifndef LOCALEDIR
#define LOCALEDIR "/usr/local/share/locale"
#endif
#define GETTEXT_PACKAGE "ejecta"

#define APP_ID      "io.github.SimonBlancoE.Ejecta"
#define APP_NAME    "Ejecta"
#define UD_NAME     "org.freedesktop.UDisks2"
#define UD_IFACE(x) "org.freedesktop.UDisks2." x
#define SNI_PATH    "/StatusNotifierItem"
#define MENU_PATH   "/MenuBar"
#define PANEL_W     420
#define PANEL_GAP   8

/* ---- gtk4-layer-shell ---------------------------------------------------- */
#if defined(NO_LAYER_SHELL)
/* Built without gtk4-layer-shell: the panel is an ordinary window. */
enum { LS_LAYER_TOP, LS_EDGE_LEFT, LS_EDGE_RIGHT, LS_EDGE_TOP, LS_EDGE_BOTTOM, LS_KB_ON_DEMAND };
static gboolean gtk_layer_is_supported(void) { return FALSE; }
static void gtk_layer_init_for_window(GtkWindow *w) {}
static void gtk_layer_set_namespace(GtkWindow *w, const char *ns) {}
static void gtk_layer_set_layer(GtkWindow *w, int layer) {}
static void gtk_layer_set_anchor(GtkWindow *w, int edge, gboolean anchor) {}
static void gtk_layer_set_margin(GtkWindow *w, int edge, int margin) {}
static void gtk_layer_set_keyboard_mode(GtkWindow *w, int mode) {}
static void gtk_layer_set_monitor(GtkWindow *w, GdkMonitor *m) {}
#elif defined(HAVE_LAYER_SHELL_HEADER)
#include <gtk4-layer-shell.h>
#define LS_LAYER_TOP     GTK_LAYER_SHELL_LAYER_TOP
#define LS_EDGE_LEFT     GTK_LAYER_SHELL_EDGE_LEFT
#define LS_EDGE_RIGHT    GTK_LAYER_SHELL_EDGE_RIGHT
#define LS_EDGE_TOP      GTK_LAYER_SHELL_EDGE_TOP
#define LS_EDGE_BOTTOM   GTK_LAYER_SHELL_EDGE_BOTTOM
#define LS_KB_ON_DEMAND  GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND
#else
/* Some distributions ship the library without its headers; the C API is
 * small and stable, so declare what we use. */
enum { LS_LAYER_TOP = 2 };
enum { LS_EDGE_LEFT = 0, LS_EDGE_RIGHT = 1, LS_EDGE_TOP = 2, LS_EDGE_BOTTOM = 3 };
enum { LS_KB_ON_DEMAND = 2 };
extern gboolean gtk_layer_is_supported(void);
extern void gtk_layer_init_for_window(GtkWindow *w);
extern void gtk_layer_set_namespace(GtkWindow *w, const char *ns);
extern void gtk_layer_set_layer(GtkWindow *w, int layer);
extern void gtk_layer_set_anchor(GtkWindow *w, int edge, gboolean anchor);
extern void gtk_layer_set_margin(GtkWindow *w, int edge, int margin);
extern void gtk_layer_set_keyboard_mode(GtkWindow *w, int mode);
extern void gtk_layer_set_monitor(GtkWindow *w, GdkMonitor *m);
#endif

/* ------------------------------------------------------------------------ */

typedef struct {
    char *path;       /* the "volume" block (the encrypted one, if encrypted) */
    char *fs_path;    /* block holding the filesystem (NULL while locked)     */
    char *drive;      /* Drive object, or NULL                                */
    char *loop;       /* Loop block it lives on (disk images), or NULL        */
    char *dev, *label, *drive_name, *fstype, *uuid, *bus, *serial, *mount;
    const char *icon;
    guint64 size;
    gboolean encrypted, locked, readonly, removable, named;
} Vol;

typedef enum { OP_MOUNT, OP_UNMOUNT, OP_UNLOCK, OP_LOCK, OP_EJECT, OP_POWEROFF, OP_LOOP_DELETE } OpKind;
typedef struct { OpKind kind; char *path, *mount; } Op;

typedef struct {
    GPtrArray *ops;
    char *vol_path;     /* affected volume (for the "busy" state)        */
    char *passphrase;
    char *what;         /* human-readable volume / device name           */
    gboolean open_after, notify_safe;
    char *mount_point, *error;
    gboolean busy_err;  /* failed because something is using it          */
    int retry;          /* action to repeat after closing the processes  */
} Job;

typedef enum { N_INSERT, N_OPEN, N_BUSY, N_INFO } NKind;
typedef struct { NKind kind; char *path; int retry; } NCtx;

typedef struct { int pid; char *name, *why; } Blocker;

enum {
    A_NONE, A_OPEN, A_MOUNT, A_UNMOUNT, A_UNLOCK, A_EJECT, A_COPY, A_DETAILS,
    A_MOUNT_ALL, A_UNMOUNT_ALL, A_EJECT_ALL, A_PANEL, A_QUIT,
    A_T_NOTIFY, A_T_ASKOPEN, A_T_INTERNAL, A_T_ICON, A_KILL_ALL, A_REFRESH
};

typedef struct {
    int id, parent, action, toggle; /* toggle: -1 none, 0/1 state */
    char *label, *icon, *arg;
    gboolean enabled, separator, submenu;
} MItem;

static struct {
    GtkApplication *app;
    GDBusConnection *session, *system;
    GDBusObjectManager *ud;
    GPtrArray *vols;
    GHashTable *known, *mounted, *busy, *errors, *no_ask, *expanded, *notifs;
    gboolean baseline;
    guint refresh_id;
    /* settings */
    gboolean notify_insert, ask_open, show_internal, always_icon;
    char *panel_pos, *file_manager;
    /* SNI */
    char *sni_name;
    const char *status;
    GPtrArray *menu;
    guint menu_rev;
    /* panel */
    GtkWindow *win;
    GtkWidget *list;
    GtkCssProvider *css;
    GFileMonitor *css_mon;
    char *unlock_target;
    guint leave_id;
    gboolean was_active, layer_ok;
} M;

/* ======================================================================== */
/* helpers                                                                  */

static char *human_size(guint64 b)
{
    return b ? g_format_size_full(b, G_FORMAT_SIZE_DEFAULT) : g_strdup("—");
}

static void vol_free(Vol *v)
{
    g_free(v->path); g_free(v->fs_path); g_free(v->drive); g_free(v->dev);
    g_free(v->label); g_free(v->drive_name); g_free(v->fstype); g_free(v->uuid);
    g_free(v->bus); g_free(v->serial); g_free(v->mount); g_free(v->loop); g_free(v);
}

static Vol *vol_find(const char *path)
{
    if (!path || !M.vols) return NULL;
    for (guint i = 0; i < M.vols->len; i++) {
        Vol *v = M.vols->pdata[i];
        if (!g_strcmp0(v->path, path)) return v;
    }
    return NULL;
}

static GVariant *uprop(const char *path, const char *iface, const char *name)
{
    if (!path || !M.ud) return NULL;
    GDBusInterface *i = g_dbus_object_manager_get_interface(M.ud, path, iface);
    if (!i) return NULL;
    GVariant *v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(i), name);
    g_object_unref(i);
    return v;
}

static gboolean has_iface(const char *path, const char *iface)
{
    GDBusInterface *i = M.ud ? g_dbus_object_manager_get_interface(M.ud, path, iface) : NULL;
    if (i) g_object_unref(i);
    return i != NULL;
}

static char *uprop_str(const char *path, const char *iface, const char *name)
{
    GVariant *v = uprop(path, iface, name);
    char *s = NULL;
    if (v) {
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING))
            s = g_strdup(g_variant_get_bytestring(v));
        else if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
                 g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
            s = g_variant_dup_string(v, NULL);
        g_variant_unref(v);
    }
    if (s && (!*s || !strcmp(s, "/"))) { g_free(s); s = NULL; }
    return s;
}

static gboolean uprop_bool(const char *path, const char *iface, const char *name)
{
    GVariant *v = uprop(path, iface, name);
    gboolean b = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
    if (v) g_variant_unref(v);
    return b;
}

static guint64 uprop_u64(const char *path, const char *iface, const char *name)
{
    GVariant *v = uprop(path, iface, name);
    guint64 n = v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64) ? g_variant_get_uint64(v) : 0;
    if (v) g_variant_unref(v);
    return n;
}

static char *first_mount(const char *path)
{
    GVariant *v = uprop(path, UD_IFACE("Filesystem"), "MountPoints");
    char *m = NULL;
    if (v) {
        if (g_variant_n_children(v) > 0) {
            GVariant *c = g_variant_get_child_value(v, 0);
            m = g_strdup(g_variant_get_bytestring(c));
            g_variant_unref(c);
        }
        g_variant_unref(v);
    }
    if (m && !*m) { g_free(m); m = NULL; }
    return m;
}

static gboolean user_mount_point(const char *m)
{
    return g_str_has_prefix(m, "/run/media/") || g_str_has_prefix(m, "/media/") ||
           g_str_has_prefix(m, "/mnt/");
}

/* ======================================================================== */
/* settings (~/.config/ejecta/ejecta.ini)                                   */

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "ejecta", "ejecta.ini", NULL);
}

static gboolean kf_bool(GKeyFile *kf, const char *key, gboolean def)
{
    GError *e = NULL;
    gboolean b = g_key_file_get_boolean(kf, "ejecta", key, &e);
    if (e) { g_error_free(e); return def; }
    return b;
}

static void config_load(void)
{
    M.notify_insert = TRUE; M.ask_open = FALSE; M.show_internal = FALSE; M.always_icon = FALSE;
    g_clear_pointer(&M.panel_pos, g_free);
    g_clear_pointer(&M.file_manager, g_free);
    g_autofree char *p = config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL)) {
        M.notify_insert = kf_bool(kf, "ask_to_mount", M.notify_insert);
        M.ask_open      = kf_bool(kf, "ask_to_open", M.ask_open);
        M.show_internal = kf_bool(kf, "show_internal", M.show_internal);
        M.always_icon   = kf_bool(kf, "always_show_icon", M.always_icon);
        M.panel_pos     = g_key_file_get_string(kf, "ejecta", "panel_position", NULL);
        M.file_manager  = g_key_file_get_string(kf, "ejecta", "file_manager", NULL);
        if (M.file_manager) g_strstrip(M.file_manager);
    }
    if (!M.panel_pos) M.panel_pos = g_strdup("auto");
    g_key_file_free(kf);
}

static void config_save(void)
{
    g_autofree char *p = config_path();
    g_autofree char *d = g_path_get_dirname(p);
    g_mkdir_with_parents(d, 0700);
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_comment(kf, NULL, NULL,
        " Ejecta settings. The booleans can also be toggled from the tray menu.\n"
        " panel_position: auto | top-left | top-right | bottom-left | bottom-right\n"
        " file_manager: command to open folders with (empty = system default)", NULL);
    g_key_file_set_boolean(kf, "ejecta", "ask_to_mount", M.notify_insert);
    g_key_file_set_boolean(kf, "ejecta", "ask_to_open", M.ask_open);
    g_key_file_set_boolean(kf, "ejecta", "show_internal", M.show_internal);
    g_key_file_set_boolean(kf, "ejecta", "always_show_icon", M.always_icon);
    g_key_file_set_string(kf, "ejecta", "panel_position", M.panel_pos);
    g_key_file_set_string(kf, "ejecta", "file_manager", M.file_manager ? M.file_manager : "");
    g_key_file_save_to_file(kf, p, NULL);
    g_key_file_free(kf);
}

/* ======================================================================== */
/* collecting volumes                                                       */

static int vol_cmp(gconstpointer a, gconstpointer b)
{
    const Vol *x = *(Vol **)a, *y = *(Vol **)b;
    if (x->removable != y->removable) return x->removable ? -1 : 1;
    return g_strcmp0(x->dev, y->dev);
}

static GPtrArray *collect_vols(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)vol_free);
    if (!M.ud) return out;
    GList *objs = g_dbus_object_manager_get_objects(M.ud);
    for (GList *l = objs; l; l = l->next) {
        const char *p = g_dbus_object_get_object_path(l->data);
        if (!has_iface(p, UD_IFACE("Block"))) continue;
        if (uprop_bool(p, UD_IFACE("Block"), "HintIgnore")) continue;
        g_autofree char *backing = uprop_str(p, UD_IFACE("Block"), "CryptoBackingDevice");
        if (backing) continue; /* unlocked cleartext is shown inside its volume */
        gboolean fs = has_iface(p, UD_IFACE("Filesystem"));
        gboolean enc = has_iface(p, UD_IFACE("Encrypted"));
        if (!fs && !enc) continue;

        Vol *v = g_new0(Vol, 1);
        v->path = g_strdup(p);
        v->drive = uprop_str(p, UD_IFACE("Block"), "Drive");
        if (has_iface(p, UD_IFACE("Loop"))) v->loop = g_strdup(p);
        else if (has_iface(p, UD_IFACE("Partition"))) {
            char *table = uprop_str(p, UD_IFACE("Partition"), "Table");
            if (table && has_iface(table, UD_IFACE("Loop"))) v->loop = table; else g_free(table);
        }
        v->encrypted = enc;
        v->size = uprop_u64(p, UD_IFACE("Block"), "Size");
        v->readonly = uprop_bool(p, UD_IFACE("Block"), "ReadOnly");
        v->dev = uprop_str(p, UD_IFACE("Block"), "PreferredDevice");
        if (!v->dev) v->dev = uprop_str(p, UD_IFACE("Block"), "Device");

        if (enc) {
            char *clear = uprop_str(p, UD_IFACE("Encrypted"), "CleartextDevice");
            if (clear && has_iface(clear, UD_IFACE("Filesystem"))) v->fs_path = clear;
            else { g_free(clear); v->locked = clear == NULL; }
            if (!v->fs_path && !v->locked) { vol_free(v); continue; } /* LVM or other content */
        } else {
            v->fs_path = g_strdup(p);
        }

        const char *meta = v->fs_path ? v->fs_path : p;
        v->label = uprop_str(meta, UD_IFACE("Block"), "IdLabel");
        v->fstype = uprop_str(meta, UD_IFACE("Block"), "IdType");
        v->uuid = uprop_str(meta, UD_IFACE("Block"), "IdUUID");
        if (v->fs_path) v->mount = first_mount(v->fs_path);

        gboolean optical = FALSE, sd = FALSE;
        if (v->drive) {
            const char *d = v->drive;
            g_autofree char *vendor = uprop_str(d, UD_IFACE("Drive"), "Vendor");
            g_autofree char *model = uprop_str(d, UD_IFACE("Drive"), "Model");
            v->drive_name = g_strstrip(g_strdup_printf("%s %s", vendor ? vendor : "", model ? model : ""));
            if (!*v->drive_name) g_clear_pointer(&v->drive_name, g_free);
            v->bus = uprop_str(d, UD_IFACE("Drive"), "ConnectionBus");
            v->serial = uprop_str(d, UD_IFACE("Drive"), "Serial");
            optical = uprop_bool(d, UD_IFACE("Drive"), "Optical");
            g_autofree char *media = uprop_str(d, UD_IFACE("Drive"), "Media");
            sd = (v->bus && !strcmp(v->bus, "sdio")) || (media && g_str_has_prefix(media, "flash"));
            v->removable = uprop_bool(d, UD_IFACE("Drive"), "Removable") ||
                           uprop_bool(d, UD_IFACE("Drive"), "MediaRemovable") ||
                           uprop_bool(d, UD_IFACE("Drive"), "Ejectable") ||
                           (v->bus && (!strcmp(v->bus, "usb") || !strcmp(v->bus, "sdio") ||
                                       !strcmp(v->bus, "ieee1394")));
        }
        if (v->loop) v->removable = TRUE;

        gboolean system = uprop_bool(p, UD_IFACE("Block"), "HintSystem");
        if (!v->removable && (!M.show_internal || (v->mount && !user_mount_point(v->mount)))) {
            vol_free(v); continue;
        }
        if (v->removable && system && v->mount && !user_mount_point(v->mount)) {
            vol_free(v); continue; /* e.g. an external disk the system runs from */
        }

        v->icon = optical ? "media-optical" : sd ? "media-flash-sd-mmc" :
                  v->loop ? "drive-removable-media" :
                  v->removable ? "drive-removable-media-usb" : "drive-harddisk";
        v->named = v->label || v->drive_name;
        if (!v->label) {
            g_autofree char *sz = human_size(v->size);
            /* TRANSLATORS: fallback names of an unlabeled volume, e.g. "16 GB Volume" */
            v->label = v->drive_name ? g_strdup(v->drive_name) :
                       v->locked ? g_strdup_printf(_("%s Encrypted Volume"), sz) :
                                   g_strdup_printf(_("%s Volume"), sz);
        }
        g_ptr_array_add(out, v);
    }
    g_list_free_full(objs, g_object_unref);
    g_ptr_array_sort(out, vol_cmp);
    return out;
}

/* ======================================================================== */
/* notifications                                                            */

static void ncx_free(NCtx *c) { g_free(c->path); g_free(c); }

static void notify_done(GObject *src, GAsyncResult *res, gpointer data)
{
    NCtx *pending = data;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (r) {
        guint32 id;
        g_variant_get(r, "(u)", &id);
        g_hash_table_replace(M.notifs, GUINT_TO_POINTER(id), pending);
        g_variant_unref(r);
    } else {
        ncx_free(pending);
    }
}

/* actions: NULL-terminated key/label pairs */
static void notify(NKind kind, const char *path, int retry, const char *icon, const char *summary,
                   const char *body, int timeout, const char *const *actions)
{
    if (!M.session) return;
    GVariantBuilder ab, hb;
    g_variant_builder_init(&ab, G_VARIANT_TYPE("as"));
    for (int i = 0; actions && actions[i]; i++) g_variant_builder_add(&ab, "s", actions[i]);
    g_variant_builder_init(&hb, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hb, "{sv}", "desktop-entry", g_variant_new_string(APP_ID));
    g_variant_builder_add(&hb, "{sv}", "category", g_variant_new_string("device"));
    NCtx *pending = g_new0(NCtx, 1);
    pending->kind = kind; pending->retry = retry; pending->path = g_strdup(path);
    g_dbus_connection_call(M.session, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susssasa{sv}i)", APP_NAME, 0u, icon ? icon : "drive-removable-media",
                      summary, body ? body : "", &ab, &hb, timeout),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, notify_done, pending);
}

static void notify_close_for(const char *path)
{
    GHashTableIter it; gpointer k, val;
    g_hash_table_iter_init(&it, M.notifs);
    while (g_hash_table_iter_next(&it, &k, &val)) {
        NCtx *c = val;
        if (c->kind != N_INFO && !g_strcmp0(c->path, path)) {
            g_dbus_connection_call(M.session, "org.freedesktop.Notifications",
                "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                "CloseNotification", g_variant_new("(u)", GPOINTER_TO_UINT(k)),
                NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
            g_hash_table_iter_remove(&it);
        }
    }
}

/* ======================================================================== */
/* jobs: UDisks operations run in order, with sync calls, in a worker thread */

static void schedule_refresh(void);
static void do_action(int action, const char *path);
static void panel_show(int x, int y);

static void job_free(Job *j)
{
    g_ptr_array_unref(j->ops);
    if (j->passphrase) { memset(j->passphrase, 0, strlen(j->passphrase)); g_free(j->passphrase); }
    g_free(j->vol_path); g_free(j->what); g_free(j->mount_point); g_free(j->error); g_free(j);
}

static void op_free(Op *o) { g_free(o->path); g_free(o->mount); g_free(o); }

static void job_add(Job *j, OpKind k, const char *path)
{
    Op *o = g_new0(Op, 1);
    o->kind = k; o->path = g_strdup(path);
    g_ptr_array_add(j->ops, o);
}

static void job_add_unmount(Job *j, const char *path, const char *mount)
{
    job_add(j, OP_UNMOUNT, path);
    ((Op *)j->ops->pdata[j->ops->len - 1])->mount = g_strdup(mount);
}

static Job *job_new(const char *vol_path, const char *what)
{
    Job *j = g_new0(Job, 1);
    j->ops = g_ptr_array_new_with_free_func((GDestroyNotify)op_free);
    j->vol_path = g_strdup(vol_path);
    j->what = g_strdup(what);
    return j;
}

static GVariant *empty_opts(void) { return g_variant_new("a{sv}", NULL); }

static char *friendly_error(GError *e)
{
    g_autofree char *remote = g_dbus_error_get_remote_error(e);
    g_dbus_error_strip_remote_error(e);
    if (remote && g_str_has_suffix(remote, ".DeviceBusy"))
        return g_strdup(_("The device is in use: close the programs and windows using it."));
    if (remote && g_str_has_suffix(remote, ".NotAuthorizedDismissed"))
        return g_strdup(_("Authorization cancelled."));
    if (remote && (g_str_has_suffix(remote, ".NotAuthorized") ||
                   g_str_has_suffix(remote, ".NotAuthorizedCanObtain")))
        return g_strdup(_("You are not allowed to do this."));
    if (e->message && strstr(e->message, "passphrase"))
        return g_strdup(_("Wrong passphrase."));
    return g_strdup(e->message);
}

static void job_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    Job *j = data;
    char *clear = NULL;
    for (guint i = 0; i < j->ops->len && !j->error; i++) {
        Op *o = j->ops->pdata[i];
        const char *path = o->path ? o->path : clear;
        const char *iface = NULL, *method = NULL;
        GVariant *args = NULL;
        const GVariantType *rtype = NULL;
        switch (o->kind) {
        case OP_MOUNT:    iface = UD_IFACE("Filesystem"); method = "Mount";   args = g_variant_new("(@a{sv})", empty_opts()); rtype = G_VARIANT_TYPE("(s)"); break;
        case OP_UNMOUNT:  iface = UD_IFACE("Filesystem"); method = "Unmount"; args = g_variant_new("(@a{sv})", empty_opts()); break;
        case OP_UNLOCK:   iface = UD_IFACE("Encrypted");  method = "Unlock";  args = g_variant_new("(s@a{sv})", j->passphrase ? j->passphrase : "", empty_opts()); rtype = G_VARIANT_TYPE("(o)"); break;
        case OP_LOCK:     iface = UD_IFACE("Encrypted");  method = "Lock";    args = g_variant_new("(@a{sv})", empty_opts()); break;
        case OP_EJECT:    iface = UD_IFACE("Drive");      method = "Eject";   args = g_variant_new("(@a{sv})", empty_opts()); break;
        case OP_POWEROFF: iface = UD_IFACE("Drive");      method = "PowerOff";args = g_variant_new("(@a{sv})", empty_opts()); break;
        case OP_LOOP_DELETE: iface = UD_IFACE("Loop");    method = "Delete";  args = g_variant_new("(@a{sv})", empty_opts()); break;
        }
        if (!path) { g_variant_unref(g_variant_ref_sink(args)); continue; }
        GError *e = NULL;
        GVariant *r = g_dbus_connection_call_sync(M.system, UD_NAME, path, iface, method, args,
                                                  rtype, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                                                  120000, NULL, &e);
        if (e) {
            g_autofree char *remote = g_dbus_error_get_remote_error(e);
            gboolean benign = remote && (g_str_has_suffix(remote, ".AlreadyMounted") ||
                                         g_str_has_suffix(remote, ".NotMounted"));
            /* Power-off fails on readers that don't support it: removal is already safe. */
            if (o->kind == OP_POWEROFF || o->kind == OP_EJECT) benign = TRUE;
            if (!benign) {
                j->busy_err = (o->kind == OP_UNMOUNT || o->kind == OP_LOCK) &&
                              ((remote && g_str_has_suffix(remote, ".DeviceBusy")) ||
                               (e->message && strstr(e->message, "busy")));
                j->error = friendly_error(e);
            }
            g_error_free(e);
            continue;
        }
        if (o->kind == OP_MOUNT) { g_free(j->mount_point); g_variant_get(r, "(s)", &j->mount_point); }
        if (o->kind == OP_UNLOCK) { g_free(clear); g_variant_get(r, "(o)", &clear); }
        g_variant_unref(r);
    }
    g_free(clear);
    g_task_return_boolean(task, TRUE);
}

static void open_path(const char *dir)
{
    if (!dir) return;
    GError *e = NULL;
    if (M.file_manager && *M.file_manager) {
        char **argv = NULL;
        if (g_shell_parse_argv(M.file_manager, NULL, &argv, &e)) {
            GStrvBuilder *b = g_strv_builder_new();
            g_strv_builder_addv(b, (const char **)argv);
            g_strv_builder_add(b, dir);
            g_auto(GStrv) full = g_strv_builder_end(b);
            g_strv_builder_unref(b);
            g_spawn_async(NULL, full, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e);
            g_strfreev(argv);
        }
    } else {
        g_autofree char *uri = g_filename_to_uri(dir, NULL, NULL);
        GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
        if (uri) g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx), &e);
        g_object_unref(ctx);
    }
    if (e) {
        g_warning("could not open %s: %s", dir, e->message);
        g_error_free(e);
    }
}

/* Whether block p is on the same physical device (or disk image) as v. */
static gboolean same_device(const char *p, Vol *v)
{
    if (v->drive) {
        g_autofree char *d = uprop_str(p, UD_IFACE("Block"), "Drive");
        return d && !strcmp(d, v->drive);
    }
    if (!v->loop) return !strcmp(p, v->path);
    if (!strcmp(p, v->loop)) return TRUE;
    g_autofree char *table = uprop_str(p, UD_IFACE("Partition"), "Table");
    return table && !strcmp(table, v->loop);
}

/* ======================================================================== */
/* processes holding a drive busy                                           */

static void blocker_free(Blocker *b) { g_free(b->name); g_free(b->why); g_free(b); }

static gboolean path_under(const char *p, char **mounts)
{
    for (int i = 0; mounts && mounts[i]; i++) {
        size_t n = strlen(mounts[i]);
        if (!strncmp(p, mounts[i], n) && (p[n] == '\0' || p[n] == '/')) return TRUE;
    }
    return FALSE;
}

static gboolean link_under(const char *link, char **mounts, char *out, size_t outsz)
{
    ssize_t n = readlink(link, out, outsz - 1);
    if (n <= 0) return FALSE;
    out[n] = '\0';
    return out[0] == '/' && path_under(out, mounts);
}

static char *proc_name(long pid)
{
    char p[64], *s = NULL;
    snprintf(p, sizeof p, "/proc/%ld/comm", pid);
    if (g_file_get_contents(p, &s, NULL, NULL)) g_strstrip(s);
    return s ? s : g_strdup("?");
}

static int blocker_cmp(gconstpointer a, gconstpointer b)
{
    return (*(Blocker **)a)->pid - (*(Blocker **)b)->pid;
}

/* Processes (readable by this user) with something open under the mount points. */
static GPtrArray *find_blockers(char **mounts)
{
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)blocker_free);
    DIR *proc = mounts && mounts[0] ? opendir("/proc") : NULL;
    if (!proc) return out;
    char buf[PATH_MAX], p[512];
    long self = getpid();
    struct dirent *de;
    while ((de = readdir(proc))) {
        char *end;
        long pid = strtol(de->d_name, &end, 10);
        if (*end || pid <= 1 || pid == self) continue;
        gboolean cwd = FALSE, exe = FALSE, mapped = FALSE;
        int nfiles = 0;
        g_autofree char *file = NULL;
        snprintf(p, sizeof p, "/proc/%ld/cwd", pid);
        cwd = link_under(p, mounts, buf, sizeof buf);
        snprintf(p, sizeof p, "/proc/%ld/exe", pid);
        exe = link_under(p, mounts, buf, sizeof buf);
        snprintf(p, sizeof p, "/proc/%ld/fd", pid);
        DIR *fd = opendir(p);
        if (fd) {
            struct dirent *f;
            while ((f = readdir(fd))) {
                if (f->d_name[0] == '.') continue;
                snprintf(p, sizeof p, "/proc/%ld/fd/%s", pid, f->d_name);
                if (link_under(p, mounts, buf, sizeof buf)) {
                    nfiles++;
                    if (!file) file = g_path_get_basename(buf);
                }
            }
            closedir(fd);
        }
        if (!cwd && !exe && !nfiles) {
            snprintf(p, sizeof p, "/proc/%ld/maps", pid);
            FILE *m = fopen(p, "r");
            if (m) {
                while (!mapped && fgets(buf, sizeof buf, m)) {
                    char *path = strchr(buf, '/');
                    if (!path) continue;
                    path[strcspn(path, "\n")] = '\0';
                    mapped = path_under(path, mounts);
                }
                fclose(m);
            }
        }
        if (!cwd && !exe && !nfiles && !mapped) continue;

        GString *why = g_string_new(NULL);
        if (nfiles == 1) g_string_append_printf(why, _("open file: %s"), file);
        else if (nfiles > 1)
            g_string_append_printf(why, ngettext("%d open file (%s…)", "%d open files (%s…)", nfiles), nfiles, file);
        if (cwd) g_string_append_printf(why, "%s%s", why->len ? "; " : "", _("working directory is on the drive"));
        if (exe) g_string_append_printf(why, "%s%s", why->len ? "; " : "", _("running from the drive"));
        if (mapped) g_string_append(why, _("has a file from the drive loaded in memory"));
        Blocker *b = g_new0(Blocker, 1);
        b->pid = (int)pid;
        b->name = proc_name(pid);
        b->why = g_string_free(why, FALSE);
        g_ptr_array_add(out, b);
    }
    closedir(proc);
    g_ptr_array_sort(out, blocker_cmp);
    return out;
}

/* Mount points of the volume or, with whole_drive, of its whole device. */
static char **vol_mounts(Vol *v, gboolean whole_drive)
{
    GPtrArray *a = g_ptr_array_new();
    if (whole_drive && (v->drive || v->loop) && M.ud) {
        GList *objs = g_dbus_object_manager_get_objects(M.ud);
        for (GList *l = objs; l; l = l->next) {
            const char *p = g_dbus_object_get_object_path(l->data);
            if (!same_device(p, v)) continue;
            g_autofree char *clear = has_iface(p, UD_IFACE("Encrypted")) ?
                                     uprop_str(p, UD_IFACE("Encrypted"), "CleartextDevice") : NULL;
            char *m = first_mount(clear ? clear : p);
            if (m) g_ptr_array_add(a, m);
        }
        g_list_free_full(objs, g_object_unref);
    } else if (v->mount) {
        g_ptr_array_add(a, g_strdup(v->mount));
    }
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

static gboolean pid_alive(int pid)
{
    char p[64], *s = NULL;
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    if (!g_file_get_contents(p, &s, NULL, NULL)) return FALSE;
    char *r = strrchr(s, ')');
    gboolean alive = r && r[1] && r[2] != 'Z' && r[2] != 'X';
    g_free(s);
    return alive;
}

typedef struct { GArray *pids; int retry, ticks; char *path; } Killer;

/* SIGTERM first; SIGKILL whatever is still alive after 3 s; then retry. */
static gboolean kill_tick(gpointer d)
{
    Killer *k = d;
    gboolean any = FALSE;
    k->ticks++;
    for (guint i = 0; i < k->pids->len; i++) {
        int pid = g_array_index(k->pids, int, i);
        if (!pid_alive(pid)) continue;
        any = TRUE;
        if (k->ticks == 12) kill(pid, SIGKILL);
    }
    if (any && k->ticks < 20) return G_SOURCE_CONTINUE;
    g_hash_table_remove(M.busy, k->path);
    if (k->retry) do_action(k->retry, k->path);
    schedule_refresh();
    g_array_unref(k->pids); g_free(k->path); g_free(k);
    return G_SOURCE_REMOVE;
}

static void kill_blockers(GPtrArray *bl, int retry, const char *path)
{
    Killer *k = g_new0(Killer, 1);
    k->pids = g_array_new(FALSE, FALSE, sizeof(int));
    k->retry = retry;
    k->path = g_strdup(path);
    GString *fail = NULL;
    for (guint i = 0; i < bl->len; i++) {
        Blocker *b = bl->pdata[i];
        if (kill(b->pid, SIGTERM) == 0) g_array_append_val(k->pids, b->pid);
        else if (errno != ESRCH) {
            if (!fail) fail = g_string_new(NULL);
            g_string_append_printf(fail, "%s%s (PID %d)", fail->len ? ", " : "", b->name, b->pid);
        }
    }
    if (fail) {
        /* TRANSLATORS: %s is a list like "foo (PID 12), bar (PID 34)" */
        g_autofree char *body = g_strdup_printf(_("%s: they belong to another user or to the system."), fail->str);
        notify(N_INFO, NULL, 0, "dialog-error", _("Some processes could not be closed"), body, 8000, NULL);
        g_string_free(fail, TRUE);
    }
    g_hash_table_add(M.busy, g_strdup(path));
    g_hash_table_remove(M.errors, path);
    schedule_refresh();
    g_timeout_add(250, kill_tick, k);
}

/* Unmounting only needs this volume free; anything else frees the whole device. */
static void kill_for_vol(Vol *v, int retry)
{
    g_auto(GStrv) ms = vol_mounts(v, retry != A_UNMOUNT);
    GPtrArray *bl = find_blockers(ms);
    if (bl->len) kill_blockers(bl, retry, v->path);
    else if (retry) do_action(retry, v->path);
    else {
        g_autofree char *body = g_strdup_printf(_("None of your processes is using “%s”."), v->label);
        notify(N_INFO, NULL, 0, "dialog-information", _("Nothing to close"), body, 5000, NULL);
    }
    g_ptr_array_unref(bl);
}

static void busy_report(Vol *v, int retry, const char *what)
{
    g_auto(GStrv) ms = vol_mounts(v, retry == A_EJECT);
    GPtrArray *bl = find_blockers(ms);
    GString *body = g_string_new(NULL);
    if (bl->len) {
        g_string_append(body, _("It is being used by:"));
        for (guint i = 0; i < bl->len && i < 6; i++) {
            Blocker *b = bl->pdata[i];
            g_string_append_printf(body, "\n• %s (PID %d): %s", b->name, b->pid, b->why);
        }
        if (bl->len > 6)
            g_string_append_printf(body, ngettext("\n… and %u more", "\n… and %u more", bl->len - 6), bl->len - 6);
    } else {
        g_string_append(body, _("None of your processes is using it: it may be a system process "
                                "or another user's (an indexer, a backup…)."));
    }
    g_hash_table_replace(M.errors, g_strdup(v->path), bl->len
        ? g_strdup_printf(ngettext("In use by %u process: see the list in the details.",
                                   "In use by %u processes: see the list in the details.", bl->len), bl->len)
        : g_strdup(_("In use by a process that isn't yours (system or another user).")));
    g_hash_table_add(M.expanded, g_strdup(v->path));
    g_autofree char *sum = g_strdup_printf(retry == A_EJECT ? _("Can't eject “%s”") : _("Can't unmount “%s”"), what);
    const char *kill_acts[] = { "kill", _("Close processes and retry"), "show", _("Show details"), NULL };
    const char *retry_acts[] = { "retry", _("Retry"), "show", _("Show details"), NULL };
    notify(N_BUSY, v->path, retry, "dialog-warning", sum, body->str, 0,
           (const char *const *)(bl->len ? kill_acts : retry_acts));
    g_string_free(body, TRUE);
    g_ptr_array_unref(bl);
}

static void job_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Job *j = data;
    g_hash_table_remove(M.busy, j->vol_path);
    Vol *v = vol_find(j->vol_path);
    if (j->error && j->busy_err && v) {
        busy_report(v, j->retry, j->what);
    } else if (j->error) {
        g_hash_table_replace(M.errors, g_strdup(j->vol_path), g_strdup(j->error));
        g_hash_table_remove(M.no_ask, j->vol_path);
        notify(N_INFO, NULL, 0, "dialog-error", j->what, j->error, 8000, NULL);
    } else {
        g_hash_table_remove(M.errors, j->vol_path);
        if (j->open_after) open_path(j->mount_point);
        if (j->notify_safe) {
            g_autofree char *b = g_strdup_printf(_("“%s” can now be safely removed."), j->what);
            notify(N_INFO, NULL, 0, "drive-removable-media", _("Device ejected"), b, 5000, NULL);
        }
    }
    job_free(j);
    schedule_refresh();
}

static void job_run(Job *j)
{
    if (!j->ops->len) { job_free(j); return; }
    g_hash_table_add(M.busy, g_strdup(j->vol_path));
    g_hash_table_remove(M.errors, j->vol_path);
    GTask *t = g_task_new(NULL, NULL, job_done, j);
    g_task_set_task_data(t, j, NULL);
    g_task_run_in_thread(t, job_thread);
    g_object_unref(t);
    schedule_refresh();
}

/* Adds to the job the unmounting and locking of everything on v's device. */
static void plan_release(Job *j, Vol *v)
{
    GList *objs = g_dbus_object_manager_get_objects(M.ud);
    GPtrArray *locks = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = objs; l; l = l->next) {
        const char *p = g_dbus_object_get_object_path(l->data);
        if (!same_device(p, v)) continue;
        if (has_iface(p, UD_IFACE("Encrypted"))) {
            g_autofree char *clear = uprop_str(p, UD_IFACE("Encrypted"), "CleartextDevice");
            if (clear) {
                g_autofree char *cm = first_mount(clear);
                if (cm) job_add_unmount(j, clear, cm);
                g_ptr_array_add(locks, g_strdup(p));
            }
        } else if (has_iface(p, UD_IFACE("Filesystem"))) {
            g_autofree char *m = first_mount(p);
            if (m) job_add_unmount(j, p, m);
        }
    }
    for (guint i = 0; i < locks->len; i++) job_add(j, OP_LOCK, locks->pdata[i]);
    g_ptr_array_unref(locks);
    g_list_free_full(objs, g_object_unref);
}

static void act_mount(Vol *v, gboolean open_after)
{
    if (g_hash_table_contains(M.busy, v->path)) return;
    if (v->mount) { if (open_after) open_path(v->mount); return; }
    if (v->locked) {
        g_free(M.unlock_target);
        M.unlock_target = g_strdup(v->path);
        panel_show(-1, -1);
        return;
    }
    Job *j = job_new(v->path, v->label);
    j->open_after = open_after;
    if (open_after) g_hash_table_add(M.no_ask, g_strdup(v->path));
    job_add(j, OP_MOUNT, v->fs_path);
    job_run(j);
}

static void act_unlock(Vol *v, const char *pass)
{
    Job *j = job_new(v->path, v->label);
    j->passphrase = g_strdup(pass);
    job_add(j, OP_UNLOCK, v->path);
    job_add(j, OP_MOUNT, NULL); /* NULL = the device that was just unlocked */
    job_run(j);
}

static void act_unmount(Vol *v)
{
    if (g_hash_table_contains(M.busy, v->path)) return;
    Job *j = job_new(v->path, v->label);
    j->retry = A_UNMOUNT;
    if (v->mount) job_add_unmount(j, v->fs_path, v->mount);
    if (v->encrypted && !v->locked) job_add(j, OP_LOCK, v->path);
    job_run(j);
}

static void act_eject(Vol *v)
{
    if (g_hash_table_contains(M.busy, v->path)) return;
    Job *j = job_new(v->path, v->drive_name ? v->drive_name : v->label);
    j->notify_safe = TRUE;
    j->retry = A_EJECT;
    plan_release(j, v);
    if (v->drive) {
        if (uprop_bool(v->drive, UD_IFACE("Drive"), "Ejectable")) job_add(j, OP_EJECT, v->drive);
        if (uprop_bool(v->drive, UD_IFACE("Drive"), "CanPowerOff")) job_add(j, OP_POWEROFF, v->drive);
    } else if (v->loop) {
        job_add(j, OP_LOOP_DELETE, v->loop);
    }
    if (!j->ops->len) {
        /* Nothing to do: not mounted and can't be powered off, so it can be pulled already. */
        g_autofree char *b = g_strdup_printf(_("“%s” is not in use: you can remove it."), j->what);
        notify(N_INFO, NULL, 0, "drive-removable-media", _("Ready to remove"), b, 5000, NULL);
        job_free(j);
        return;
    }
    job_run(j);
}

/* ======================================================================== */
/* panel (left click)                                                       */

static void panel_hide(void)
{
    if (M.leave_id) { g_source_remove(M.leave_id); M.leave_id = 0; }
    if (M.win && gtk_widget_get_visible(GTK_WIDGET(M.win))) gtk_widget_set_visible(GTK_WIDGET(M.win), FALSE);
    M.was_active = FALSE;
}

static void on_btn(GtkButton *b, gpointer action)
{
    const char *path = g_object_get_data(G_OBJECT(b), "path");
    do_action(GPOINTER_TO_INT(action), path);
}

static GtkWidget *mk_button(const char *label, const char *icon, const char *tip, int action,
                            const char *path, gboolean primary)
{
    GtkWidget *b = label ? gtk_button_new_with_label(label) : gtk_button_new_from_icon_name(icon);
    if (label && icon) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
        gtk_box_append(GTK_BOX(box), gtk_label_new(label));
        gtk_button_set_child(GTK_BUTTON(b), box);
    }
    gtk_widget_add_css_class(b, primary ? "m-primary" : "m-btn");
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    g_object_set_data_full(G_OBJECT(b), "path", g_strdup(path), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(on_btn), GINT_TO_POINTER(action));
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    return b;
}

static GtkWidget *mk_label(const char *text, const char *cls, gboolean wrap)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (wrap) {
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 1); /* take the allocated width, no more */
        gtk_widget_set_hexpand(l, TRUE);
    } else {
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
    }
    if (cls) gtk_widget_add_css_class(l, cls);
    return l;
}

static void on_toggle_details(GtkToggleButton *b, gpointer rev)
{
    const char *path = g_object_get_data(G_OBJECT(b), "path");
    gboolean on = gtk_toggle_button_get_active(b);
    if (on) g_hash_table_add(M.expanded, g_strdup(path)); else g_hash_table_remove(M.expanded, path);
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), on);
    Vol *v = vol_find(path);
    if (on && v && v->mount) schedule_refresh(); /* fills in the process list */
}

static void on_unlock_activate(GtkWidget *entry, gpointer data)
{
    const char *path = g_object_get_data(G_OBJECT(entry), "path");
    Vol *v = vol_find(path);
    const char *pass = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!v || !pass || !*pass) return;
    g_clear_pointer(&M.unlock_target, g_free);
    act_unlock(v, pass);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void on_unlock_cancel(GtkButton *b, gpointer d)
{
    g_clear_pointer(&M.unlock_target, g_free);
    schedule_refresh();
}

static void details_row(GtkGrid *g, int *row, const char *key, const char *val)
{
    if (!val || !*val) return;
    GtkWidget *k = mk_label(key, "m-key", FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(k), PANGO_ELLIPSIZE_NONE);
    gtk_label_set_max_width_chars(GTK_LABEL(k), -1);
    gtk_widget_set_valign(k, GTK_ALIGN_START);
    GtkWidget *v = mk_label(val, "m-val", TRUE);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(g, k, 0, *row, 1, 1);
    gtk_grid_attach(g, v, 1, *row, 1, 1);
    (*row)++;
}

static const char *bus_name(const char *bus)
{
    if (!bus) return NULL;
    if (!strcmp(bus, "usb")) return "USB";
    if (!strcmp(bus, "sdio")) return _("SD card reader");
    if (!strcmp(bus, "ieee1394")) return "FireWire";
    if (!strcmp(bus, "scsi")) return "SCSI / SATA";
    return bus;
}

static void on_kill_pid(GtkButton *b, gpointer d)
{
    GPtrArray *bl = g_ptr_array_new_with_free_func((GDestroyNotify)blocker_free);
    Blocker *one = g_new0(Blocker, 1);
    one->pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "pid"));
    one->name = proc_name(one->pid);
    g_ptr_array_add(bl, one);
    kill_blockers(bl, 0, g_object_get_data(G_OBJECT(b), "path"));
    g_ptr_array_unref(bl);
}

/* List of the processes using the drive, with buttons to close them. */
static GtkWidget *procs_section(Vol *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 8);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hl = mk_label(_("Processes using this device"), "m-key", FALSE);
    gtk_label_set_max_width_chars(GTK_LABEL(hl), -1);
    gtk_widget_set_hexpand(hl, TRUE);
    gtk_box_append(GTK_BOX(head), hl);
    GtkWidget *upd = mk_button(NULL, "view-refresh", _("Look again"), A_REFRESH, v->path, FALSE);
    gtk_widget_add_css_class(upd, "m-small");
    gtk_box_append(GTK_BOX(head), upd);
    gtk_box_append(GTK_BOX(box), head);

    g_auto(GStrv) ms = vol_mounts(v, TRUE);
    GPtrArray *bl = find_blockers(ms);
    if (!bl->len)
        gtk_box_append(GTK_BOX(box), mk_label(_("None of your processes is using it."), "m-dim", TRUE));
    for (guint i = 0; i < bl->len; i++) {
        Blocker *b = bl->pdata[i];
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(row, "m-proc");
        g_autofree char *name = g_markup_printf_escaped("<b>%s</b>  <small>PID %d</small>", b->name, b->pid);
        GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(txt, TRUE);
        GtkWidget *nl = mk_label(NULL, NULL, FALSE);
        gtk_label_set_markup(GTK_LABEL(nl), name);
        gtk_box_append(GTK_BOX(txt), nl);
        gtk_box_append(GTK_BOX(txt), mk_label(b->why, "m-dim", TRUE));
        gtk_box_append(GTK_BOX(row), txt);
        GtkWidget *k = gtk_button_new_with_label(_("Close"));
        gtk_widget_add_css_class(k, "m-danger");
        gtk_widget_set_valign(k, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(k, _("Asks the process to quit; if it doesn't within 3 s, it is forced"));
        g_object_set_data(G_OBJECT(k), "pid", GINT_TO_POINTER(b->pid));
        g_object_set_data_full(G_OBJECT(k), "path", g_strdup(v->path), g_free);
        g_signal_connect(k, "clicked", G_CALLBACK(on_kill_pid), NULL);
        gtk_box_append(GTK_BOX(row), k);
        gtk_box_append(GTK_BOX(box), row);
    }
    if (bl->len > 1) {
        GtkWidget *all = mk_button(_("Close all"), NULL,
            _("Asks them all to quit; the ones still running after 3 s are forced"), A_KILL_ALL, v->path, FALSE);
        gtk_widget_add_css_class(all, "m-danger");
        gtk_widget_set_halign(all, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(box), all);
    }
    g_ptr_array_unref(bl);
    return box;
}

static GtkWidget *vol_card(Vol *v)
{
    gboolean busy = g_hash_table_contains(M.busy, v->path);
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "m-card");
    if (v->mount) gtk_widget_add_css_class(card, "mounted");

    /* header: icon · name and state · details toggle */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *img = gtk_image_new_from_icon_name(v->icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    gtk_box_append(GTK_BOX(top), img);

    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(txt, TRUE);
    gtk_widget_set_valign(txt, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(txt), mk_label(v->label, "m-name", FALSE));
    g_autofree char *sz = human_size(v->size);
    const char *state = busy ? _("working…") : v->locked ? _("encrypted, locked") :
                        v->mount ? _("mounted") : _("not mounted");
    g_autofree char *sub = g_strdup_printf("%s · %s%s%s", sz, state,
                                           v->fstype ? " · " : "", v->fstype ? v->fstype : "");
    gtk_box_append(GTK_BOX(txt), mk_label(sub, "m-dim", FALSE));
    gtk_box_append(GTK_BOX(top), txt);

    GtkWidget *info = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(info), "dialog-information");
    gtk_widget_add_css_class(info, "m-btn");
    gtk_widget_set_valign(info, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(info, _("Details"));
    gtk_box_append(GTK_BOX(top), info);
    gtk_box_append(GTK_BOX(card), top);

    /* actions */
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(btns), TRUE);
    if (busy) {
        GtkWidget *sp = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(sp));
        gtk_box_set_homogeneous(GTK_BOX(btns), FALSE);
        gtk_box_append(GTK_BOX(btns), sp);
    } else if (v->locked) {
        gtk_box_append(GTK_BOX(btns), mk_button(_("Unlock"), "dialog-password",
            _("Ask for the passphrase, unlock and mount"), A_UNLOCK, v->path, TRUE));
    } else if (v->mount) {
        gtk_box_append(GTK_BOX(btns), mk_button(_("Open"), "folder-open",
            _("Open in the file manager"), A_OPEN, v->path, TRUE));
        gtk_box_append(GTK_BOX(btns), mk_button(_("Unmount"), NULL,
            _("Closes the volume (its files stop being accessible) but the device stays connected and powered"),
            A_UNMOUNT, v->path, FALSE));
    } else {
        gtk_box_append(GTK_BOX(btns), mk_button(_("Mount"), NULL, _("Make the files accessible"), A_MOUNT, v->path, TRUE));
        gtk_box_append(GTK_BOX(btns), mk_button(_("Mount & open"), "folder-open",
            _("Mount and open in the file manager"), A_OPEN, v->path, FALSE));
    }
    if (!busy && (v->drive || v->loop))
        gtk_box_append(GTK_BOX(btns), mk_button(_("Eject"), "media-eject",
            _("Unmounts everything on the device and powers it off so you can remove it safely"),
            A_EJECT, v->path, FALSE));
    gtk_box_append(GTK_BOX(card), btns);

    /* usage */
    struct statvfs st;
    if (v->mount && statvfs(v->mount, &st) == 0 && st.f_blocks) {
        guint64 total = (guint64)st.f_blocks * st.f_frsize, avail = (guint64)st.f_bavail * st.f_frsize;
        guint64 used = total - (guint64)st.f_bfree * st.f_frsize;
        GtkWidget *bar = gtk_level_bar_new_for_interval(0, 1);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), total ? (double)used / total : 0);
        gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_LOW);
        gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_HIGH);
        gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_FULL);
        /* a block takes the name of the lowest offset at or above the value */
        gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(bar), "m-ok", 0.9);
        gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(bar), "m-full", 1.0);
        gtk_box_append(GTK_BOX(card), bar);
        g_autofree char *fa = human_size(avail), *ft = human_size(total);
        /* TRANSLATORS: "<free> free of <total>  ·  <mount point>" */
        g_autofree char *u = g_strdup_printf(_("%s free of %s  ·  %s"), fa, ft, v->mount);
        GtkWidget *ul = mk_label(u, "m-dim", FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(ul), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(card), ul);
    }

    /* passphrase */
    if (v->locked && !busy && !g_strcmp0(M.unlock_target, v->path)) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *pe = gtk_password_entry_new();
        gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(pe), TRUE);
        g_object_set(pe, "placeholder-text", _("Passphrase, then Enter"), "activates-default", FALSE, NULL);
        gtk_widget_set_hexpand(pe, TRUE);
        g_object_set_data_full(G_OBJECT(pe), "path", g_strdup(v->path), g_free);
        g_signal_connect(pe, "activate", G_CALLBACK(on_unlock_activate), NULL);
        GtkWidget *cancel = gtk_button_new_from_icon_name("window-close");
        gtk_widget_add_css_class(cancel, "m-btn");
        g_signal_connect(cancel, "clicked", G_CALLBACK(on_unlock_cancel), NULL);
        gtk_box_append(GTK_BOX(row), pe);
        gtk_box_append(GTK_BOX(row), cancel);
        gtk_box_append(GTK_BOX(card), row);
        g_object_set_data(G_OBJECT(M.win), "focus", pe);
    }

    /* error from the last operation */
    const char *err = g_hash_table_lookup(M.errors, v->path);
    if (err) gtk_box_append(GTK_BOX(card), mk_label(err, "m-error", TRUE));

    /* details */
    GtkWidget *rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    gtk_widget_add_css_class(grid, "m-details");
    int r = 0;
    details_row(GTK_GRID(grid), &r, _("Device"), v->drive_name);
    details_row(GTK_GRID(grid), &r, _("Connection"), v->loop ? _("Disk image (loop)") : bus_name(v->bus));
    details_row(GTK_GRID(grid), &r, _("Path"), v->dev);
    details_row(GTK_GRID(grid), &r, _("Mounted at"), v->mount);
    details_row(GTK_GRID(grid), &r, _("Filesystem"), v->fstype);
    details_row(GTK_GRID(grid), &r, "UUID", v->uuid);
    details_row(GTK_GRID(grid), &r, _("Serial number"), v->serial);
    details_row(GTK_GRID(grid), &r, _("Size"), sz);
    details_row(GTK_GRID(grid), &r, _("Encryption"),
                v->encrypted ? (v->locked ? _("LUKS, locked") : _("LUKS, unlocked")) : NULL);
    details_row(GTK_GRID(grid), &r, _("Access"), v->readonly ? _("Read-only") : NULL);
    if (v->mount && !busy && g_hash_table_contains(M.expanded, v->path))
        gtk_grid_attach(GTK_GRID(grid), procs_section(v), 0, r++, 2, 1);
    if (v->mount) {
        GtkWidget *copy = mk_button(_("Copy path"), "edit-copy", NULL, A_COPY, v->path, FALSE);
        gtk_widget_set_halign(copy, GTK_ALIGN_START);
        gtk_widget_set_margin_top(copy, 4);
        gtk_grid_attach(GTK_GRID(grid), copy, 1, r++, 1, 1);
    }
    gtk_revealer_set_child(GTK_REVEALER(rev), grid);
    gboolean exp = g_hash_table_contains(M.expanded, v->path);
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), exp);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(info), exp);
    g_object_set_data_full(G_OBJECT(info), "path", g_strdup(v->path), g_free);
    g_signal_connect(info, "toggled", G_CALLBACK(on_toggle_details), rev);
    gtk_box_append(GTK_BOX(card), rev);
    return card;
}

static void panel_fill(void)
{
    if (!M.win) return;
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(M.list))) gtk_box_remove(GTK_BOX(M.list), c);
    g_object_set_data(G_OBJECT(M.win), "focus", NULL);
    if (!M.vols->len) {
        GtkWidget *e = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(e, "m-empty");
        GtkWidget *img = gtk_image_new_from_icon_name("drive-removable-media");
        gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
        gtk_widget_set_opacity(img, 0.6);
        gtk_box_append(GTK_BOX(e), img);
        GtkWidget *l = gtk_label_new(_("No removable drives connected"));
        gtk_widget_add_css_class(l, "m-dim");
        gtk_box_append(GTK_BOX(e), l);
        gtk_box_append(GTK_BOX(M.list), e);
    }
    for (guint i = 0; i < M.vols->len; i++) gtk_box_append(GTK_BOX(M.list), vol_card(M.vols->pdata[i]));
    GtkWidget *f = g_object_get_data(G_OBJECT(M.win), "focus");
    if (f) gtk_widget_grab_focus(f);
}

static gboolean leave_timeout(gpointer d)
{
    M.leave_id = 0;
    if (M.win && !gtk_window_is_active(M.win) && !M.unlock_target) panel_hide();
    return G_SOURCE_REMOVE;
}

static void on_enter(GtkEventControllerMotion *c, double x, double y, gpointer d)
{
    if (M.leave_id) { g_source_remove(M.leave_id); M.leave_id = 0; }
}

static void on_leave(GtkEventControllerMotion *c, gpointer d)
{
    if (M.leave_id) g_source_remove(M.leave_id);
    M.leave_id = g_timeout_add(900, leave_timeout, NULL);
}

static void on_active(GObject *w, GParamSpec *ps, gpointer d)
{
    if (gtk_window_is_active(M.win)) M.was_active = TRUE;
    else if (M.was_active) panel_hide();
}

static gboolean on_key(GtkEventControllerKey *c, guint key, guint code, GdkModifierType st, gpointer d)
{
    if (key == GDK_KEY_Escape) {
        g_clear_pointer(&M.unlock_target, g_free);
        panel_hide();
        return TRUE;
    }
    return FALSE;
}

static void panel_build(void)
{
    M.win = GTK_WINDOW(gtk_window_new());
    gtk_window_set_application(M.win, M.app);
    gtk_window_set_title(M.win, APP_NAME);
    gtk_window_set_decorated(M.win, FALSE);
    gtk_window_set_resizable(M.win, FALSE);
    gtk_window_set_hide_on_close(M.win, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(M.win), "ejecta");
    M.layer_ok = gtk_layer_is_supported();
    if (M.layer_ok) {
        gtk_layer_init_for_window(M.win);
        gtk_layer_set_namespace(M.win, "ejecta");
        gtk_layer_set_layer(M.win, LS_LAYER_TOP);
        gtk_layer_set_keyboard_mode(M.win, LS_KB_ON_DEMAND);
    }

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(panel, "m-panel");
    gtk_widget_set_size_request(panel, PANEL_W, -1);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title = mk_label(_("Drives"), "m-title", FALSE);
    gtk_label_set_max_width_chars(GTK_LABEL(title), -1);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(head), title);
    GtkWidget *all = mk_button(NULL, "media-eject", _("Eject all removable drives"), A_EJECT_ALL, NULL, FALSE);
    g_object_set_data(G_OBJECT(M.win), "eject-all", all);
    gtk_box_append(GTK_BOX(head), all);
    gtk_box_append(GTK_BOX(panel), head);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 560);
    M.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), M.list);
    gtk_box_append(GTK_BOX(panel), scroll);

    gtk_box_append(GTK_BOX(panel), mk_label(
        _("Unmount closes the volume; the device stays connected. "
          "Eject (⏏) unmounts everything and powers it off: then you can pull it out."), "m-hint", TRUE));

    gtk_window_set_child(M.win, panel);

    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(GTK_WIDGET(M.win), k);
    GtkEventController *mo = gtk_event_controller_motion_new();
    g_signal_connect(mo, "enter", G_CALLBACK(on_enter), NULL);
    g_signal_connect(mo, "leave", G_CALLBACK(on_leave), NULL);
    gtk_widget_add_controller(GTK_WIDGET(M.win), mo);
    g_signal_connect(M.win, "notify::is-active", G_CALLBACK(on_active), NULL);
}

/* Places the panel next to the tray: below a top bar, above a bottom one,
 * horizontally centred on the icon. Without click coordinates (some trays
 * don't send them) it uses the configured corner, top-right by default. */
static void panel_place(int x, int y)
{
    GdkMonitor *mon = NULL;
    GdkRectangle g = {0};
    if (x >= 0 && y >= 0) {
        GListModel *ms = gdk_display_get_monitors(gdk_display_get_default());
        for (guint i = 0; i < g_list_model_get_n_items(ms) && !mon; i++) {
            GdkMonitor *m = g_list_model_get_item(ms, i);
            gdk_monitor_get_geometry(m, &g);
            if (x >= g.x && x < g.x + g.width && y >= g.y && y < g.y + g.height) mon = m;
            g_object_unref(m);
        }
    }
    gboolean fixed = g_strcmp0(M.panel_pos, "auto") != 0;
    if (!mon && !fixed && gtk_widget_get_visible(GTK_WIDGET(M.win))) return; /* keep where it is */

    gboolean bottom = fixed ? g_str_has_prefix(M.panel_pos, "bottom") : mon && (y - g.y) > g.height / 2;
    gboolean left = fixed && g_str_has_suffix(M.panel_pos, "left");
    gtk_layer_set_monitor(M.win, mon);
    gtk_layer_set_anchor(M.win, LS_EDGE_TOP, !bottom);
    gtk_layer_set_anchor(M.win, LS_EDGE_BOTTOM, bottom);
    gtk_layer_set_margin(M.win, bottom ? LS_EDGE_BOTTOM : LS_EDGE_TOP, PANEL_GAP);
    if (mon && !fixed) {
        int margin = CLAMP(x - g.x - PANEL_W / 2, PANEL_GAP, MAX(PANEL_GAP, g.width - PANEL_W - PANEL_GAP));
        gtk_layer_set_anchor(M.win, LS_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(M.win, LS_EDGE_RIGHT, FALSE);
        gtk_layer_set_margin(M.win, LS_EDGE_LEFT, margin);
    } else {
        gtk_layer_set_anchor(M.win, LS_EDGE_LEFT, left);
        gtk_layer_set_anchor(M.win, LS_EDGE_RIGHT, !left);
        gtk_layer_set_margin(M.win, left ? LS_EDGE_LEFT : LS_EDGE_RIGHT, PANEL_GAP);
    }
}

static void panel_show(int x, int y)
{
    if (!M.win) panel_build();
    panel_fill();
    GtkWidget *all = g_object_get_data(G_OBJECT(M.win), "eject-all");
    gtk_widget_set_visible(all, M.vols->len > 0);
    if (M.layer_ok) panel_place(x, y);
    M.was_active = FALSE;
    gtk_window_present(M.win);
}

static void panel_toggle(int x, int y)
{
    if (M.win && gtk_widget_get_visible(GTK_WIDGET(M.win))) panel_hide();
    else panel_show(x, y);
}

/* ======================================================================== */
/* style: follows the GTK theme; if ~/.config/gtk-4.0/colors.css has KDE-style
 * *_breeze colors (written by KDE and by Noctalia) those are used instead,
 * and reloaded when they change.                                           */

static const char *CSS_DEFAULTS =
    "@define-color m_bg #1e1e24; @define-color m_fg #eeeeee; @define-color m_muted #a9a9b3;"
    "@define-color m_accent #62a0ea; @define-color m_accent_fg #ffffff; @define-color m_border #3a3a44;"
    "@define-color m_error #f66151;\n";

/* GTK4's built-in theme defines these; if a theme doesn't, the defaults stay. */
static const char *CSS_THEME =
    "@define-color m_bg @theme_bg_color; @define-color m_fg @theme_fg_color;"
    "@define-color m_muted alpha(@theme_fg_color, 0.7); @define-color m_accent @theme_selected_bg_color;"
    "@define-color m_accent_fg @theme_selected_fg_color; @define-color m_border @borders;"
    "@define-color m_error @error_color;\n";

static const char *CSS_BREEZE =
    "@define-color m_bg @theme_bg_color_breeze; @define-color m_fg @theme_text_color_breeze;"
    "@define-color m_muted @theme_fg_color_breeze; @define-color m_accent @theme_selected_bg_color_breeze;"
    "@define-color m_accent_fg @theme_selected_fg_color_breeze; @define-color m_border @borders_breeze;"
    "@define-color m_error @error_color_breeze;\n";

static const char *CSS_RULES =
    "window.ejecta { background: transparent; }"
    ".m-panel { background: @m_bg; color: @m_fg; border: 1px solid @m_border; border-radius: 18px;"
    "  padding: 14px; margin: 2px; }"
    ".m-title { font-weight: 800; font-size: 1.15em; margin-left: 4px; }"
    ".m-card { background: alpha(@m_fg, 0.045); border-radius: 13px; padding: 10px 12px; }"
    ".m-card:hover { background: alpha(@m_fg, 0.075); }"
    ".m-card.mounted { box-shadow: inset 3px 0 0 @m_accent; }"
    ".m-name { font-weight: 700; }"
    ".m-dim { color: @m_muted; font-size: 0.9em; }"
    ".m-hint { color: alpha(@m_muted, 0.8); font-size: 0.82em; margin: 0 4px; }"
    ".m-empty { padding: 22px; }"
    ".m-error { color: @m_error; font-size: 0.9em; }"
    ".m-details { padding-top: 6px; border-top: 1px solid alpha(@m_fg, 0.08); }"
    ".m-key { color: @m_muted; font-size: 0.88em; }"
    ".m-val { font-size: 0.88em; }"
    "button.m-btn, button.m-primary { border-radius: 9px; border: none; box-shadow: none;"
    "  min-height: 28px; padding: 2px 10px; }"
    "button.m-btn { background: alpha(@m_fg, 0.08); color: @m_fg; }"
    "button.m-btn:hover { background: alpha(@m_fg, 0.15); }"
    "button.m-btn:checked { background: alpha(@m_accent, 0.25); }"
    "button.m-danger { border-radius: 9px; border: none; box-shadow: none; min-height: 26px; padding: 2px 10px;"
    "  background: alpha(@m_error, 0.16); color: @m_error; }"
    "button.m-danger:hover { background: alpha(@m_error, 0.28); }"
    "button.m-small { min-height: 22px; min-width: 22px; padding: 0 4px; }"
    ".m-proc { padding: 4px 0; }"
    "button.m-primary { background: @m_accent; color: @m_accent_fg; font-weight: 700; }"
    "button.m-primary:hover { background: shade(@m_accent, 1.08); }"
    "levelbar trough { border-radius: 4px; min-height: 6px; background: alpha(@m_fg, 0.1); border: none; }"
    "levelbar block { border-radius: 4px; min-height: 6px; border: none; }"
    "levelbar block.m-ok { background: @m_accent; }"
    "levelbar block.m-full { background: @m_error; }"
    "levelbar block.empty { background: transparent; }";

static char *css_colors_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtk-4.0", "colors.css", NULL);
}

static void css_load(void)
{
    g_autofree char *path = css_colors_path();
    g_autofree char *colors = NULL;
    GString *s = g_string_new(CSS_DEFAULTS);
    if (g_file_get_contents(path, &colors, NULL, NULL) && strstr(colors, "theme_selected_bg_color_breeze")) {
        g_string_append(s, colors);
        g_string_append_c(s, '\n');
        g_string_append(s, CSS_BREEZE);
    } else {
        g_string_append(s, CSS_THEME);
    }
    g_string_append(s, CSS_RULES);
    if (!M.css) {
        M.css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(M.css),
                                                   GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
    }
    gtk_css_provider_load_from_string(M.css, s->str);
    g_string_free(s, TRUE);
}

static void on_css_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer d)
{
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_CREATED ||
        ev == G_FILE_MONITOR_EVENT_DELETED)
        css_load();
}

/* ======================================================================== */
/* dbusmenu (right click)                                                   */

static void mitem_free(MItem *m) { g_free(m->label); g_free(m->icon); g_free(m->arg); g_free(m); }

static MItem *madd(int parent, const char *label, const char *icon, int action, const char *arg)
{
    MItem *m = g_new0(MItem, 1);
    m->id = M.menu->len + 1; m->parent = parent; m->action = action; m->toggle = -1;
    m->label = g_strdup(label); m->icon = g_strdup(icon); m->arg = g_strdup(arg);
    m->enabled = action != A_NONE;
    g_ptr_array_add(M.menu, m);
    return m;
}

static void msep(int parent) { MItem *m = madd(parent, NULL, NULL, A_NONE, NULL); m->separator = TRUE; }

static void mtoggle(int parent, const char *label, int action, gboolean on)
{
    MItem *m = madd(parent, label, NULL, action, NULL);
    m->toggle = on ? 1 : 0;
}

static void menu_build(void)
{
    if (M.menu) g_ptr_array_unref(M.menu);
    M.menu = g_ptr_array_new_with_free_func((GDestroyNotify)mitem_free);
    gboolean any_unmounted = FALSE, any_mounted = FALSE, any_eject = FALSE;
    for (guint i = 0; i < M.vols->len; i++) {
        Vol *v = M.vols->pdata[i];
        gboolean busy = g_hash_table_contains(M.busy, v->path);
        g_autofree char *sz = human_size(v->size);
        const char *state = busy ? _("working…") : v->mount ? _("mounted") : v->locked ? _("locked") : NULL;
        g_autofree char *name = v->named ? g_strdup_printf("%s  —  %s", v->label, sz) : g_strdup(v->label);
        g_autofree char *lbl = state ? g_strdup_printf("%s (%s)", name, state) : g_strdup(name);
        MItem *sub = madd(0, lbl, v->icon, A_NONE, NULL);
        sub->enabled = TRUE; sub->submenu = TRUE;
        int p = sub->id;
        if (v->mount) any_mounted = TRUE; else if (!v->locked) any_unmounted = TRUE;
        if (v->drive || v->loop) any_eject = TRUE;
        if (busy) { madd(p, _("Operation in progress…"), NULL, A_NONE, NULL); continue; }
        if (v->locked) madd(p, _("Unlock and mount…"), "dialog-password", A_UNLOCK, v->path);
        else madd(p, _("Open in the file manager"), "folder-open", A_OPEN, v->path);
        if (v->mount) {
            madd(p, _("Unmount (stays connected)"), "media-eject", A_UNMOUNT, v->path);
            madd(p, _("Copy mount path"), "edit-copy", A_COPY, v->path);
        } else if (!v->locked) {
            madd(p, _("Mount"), "drive-removable-media", A_MOUNT, v->path);
        }
        if (v->encrypted && !v->locked && !v->mount) madd(p, _("Lock"), "changes-prevent", A_UNMOUNT, v->path);
        if (v->drive || v->loop) madd(p, _("Eject to remove it"), "media-eject", A_EJECT, v->path);
        msep(p);
        madd(p, _("Show details in the panel"), "dialog-information", A_DETAILS, v->path);
        if (v->mount) madd(p, _("Close the processes using it"), "process-stop", A_KILL_ALL, v->path);
        if (v->dev) { g_autofree char *t = g_strdup_printf("%s%s%s", v->dev, v->fstype ? " · " : "", v->fstype ? v->fstype : ""); madd(p, t, NULL, A_NONE, NULL); }
        if (v->mount) madd(p, v->mount, NULL, A_NONE, NULL);
        if (v->drive_name && g_strcmp0(v->drive_name, v->label)) madd(p, v->drive_name, NULL, A_NONE, NULL);
    }
    if (!M.vols->len) madd(0, _("No removable drives"), NULL, A_NONE, NULL);
    msep(0);
    madd(0, _("Mount all"), "drive-removable-media", any_unmounted ? A_MOUNT_ALL : A_NONE, NULL);
    madd(0, _("Unmount all"), NULL, any_mounted ? A_UNMOUNT_ALL : A_NONE, NULL);
    madd(0, _("Eject all"), "media-eject", any_eject ? A_EJECT_ALL : A_NONE, NULL);
    msep(0);
    MItem *pref = madd(0, _("Settings"), "preferences-system", A_NONE, NULL);
    pref->enabled = TRUE; pref->submenu = TRUE;
    mtoggle(pref->id, _("Ask to mount when a device is plugged in"), A_T_NOTIFY, M.notify_insert);
    mtoggle(pref->id, _("Ask to open the file manager after mounting"), A_T_ASKOPEN, M.ask_open);
    mtoggle(pref->id, _("Also show internal disks"), A_T_INTERNAL, M.show_internal);
    mtoggle(pref->id, _("Show the icon even with no drives"), A_T_ICON, M.always_icon);
    madd(0, _("Open panel"), "view-list-details", A_PANEL, NULL);
    madd(0, _("Quit Ejecta"), "application-exit", A_QUIT, NULL);
}

static GVariant *mprops(MItem *m, const char *const *want)
{
#define WANT(n) (!want || !want[0] || g_strv_contains(want, n))
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    if (m->separator) {
        if (WANT("type")) g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator"));
        return g_variant_builder_end(&b);
    }
    if (m->label && WANT("label")) {
        /* dbusmenu treats '_' as a mnemonic: double it to show it literally */
        char **parts = g_strsplit(m->label, "_", -1);
        g_autofree char *esc = g_strjoinv("__", parts);
        g_strfreev(parts);
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string(esc));
    }
    if (m->icon && WANT("icon-name")) g_variant_builder_add(&b, "{sv}", "icon-name", g_variant_new_string(m->icon));
    if (!m->enabled && WANT("enabled")) g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(FALSE));
    if (m->submenu && WANT("children-display")) g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
    if (m->toggle >= 0) {
        if (WANT("toggle-type")) g_variant_builder_add(&b, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
        if (WANT("toggle-state")) g_variant_builder_add(&b, "{sv}", "toggle-state", g_variant_new_int32(m->toggle));
    }
    return g_variant_builder_end(&b);
#undef WANT
}

static MItem *mget(int id) { return id >= 1 && (guint)id <= M.menu->len ? M.menu->pdata[id - 1] : NULL; }

static GVariant *mlayout(int id, int depth, const char *const *want)
{
    GVariantBuilder kids;
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    if (depth != 0)
        for (guint i = 0; i < M.menu->len; i++) {
            MItem *m = M.menu->pdata[i];
            if (m->parent == id) g_variant_builder_add(&kids, "v", mlayout(m->id, depth - 1, want));
        }
    GVariant *props;
    if (id == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
        props = g_variant_builder_end(&b);
    } else props = mprops(mget(id), want);
    return g_variant_new("(i@a{sv}av)", id, props, &kids);
}

typedef struct { int action; char *arg; } Deferred;

static gboolean run_deferred(gpointer d)
{
    Deferred *x = d;
    do_action(x->action, x->arg);
    g_free(x->arg); g_free(x);
    return G_SOURCE_REMOVE;
}

static void menu_event(int id, const char *ev)
{
    MItem *m = mget(id);
    if (!m || strcmp(ev, "clicked") || m->action == A_NONE) return;
    Deferred *x = g_new0(Deferred, 1);
    x->action = m->action; x->arg = g_strdup(m->arg);
    g_idle_add(run_deferred, x);
}

static void menu_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *method, GVariant *p, GDBusMethodInvocation *inv, gpointer d)
{
    if (!strcmp(method, "GetLayout")) {
        int parent, depth; g_autofree const char **want = NULL;
        g_variant_get(p, "(ii^a&s)", &parent, &depth, &want);
        if (parent != 0 && !mget(parent)) { g_dbus_method_invocation_return_dbus_error(inv, "com.canonical.dbusmenu.Error", "no such item"); return; }
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u@(ia{sv}av))", M.menu_rev, mlayout(parent, depth, want)));
    } else if (!strcmp(method, "GetGroupProperties")) {
        GVariantIter *ids; g_autofree const char **want = NULL; int id;
        g_variant_get(p, "(ai^a&s)", &ids, &want);
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
        gboolean all = g_variant_iter_n_children(ids) == 0;
        if (all) for (guint i = 0; i < M.menu->len; i++) g_variant_builder_add(&b, "(i@a{sv})", i + 1, mprops(M.menu->pdata[i], want));
        else while (g_variant_iter_next(ids, "i", &id)) { MItem *m = mget(id); if (m) g_variant_builder_add(&b, "(i@a{sv})", id, mprops(m, want)); }
        g_variant_iter_free(ids);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ia{sv}))", &b));
    } else if (!strcmp(method, "GetProperty")) {
        int id; const char *name;
        g_variant_get(p, "(i&s)", &id, &name);
        MItem *m = mget(id);
        GVariant *props = m ? mprops(m, NULL) : NULL;
        GVariant *v = props ? g_variant_lookup_value(props, name, NULL) : NULL;
        if (props) g_variant_unref(g_variant_ref_sink(props));
        if (v) g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
        else g_dbus_method_invocation_return_dbus_error(inv, "com.canonical.dbusmenu.Error", "no such property");
        if (v) g_variant_unref(v);
    } else if (!strcmp(method, "Event")) {
        int id; const char *ev; GVariant *data; guint32 ts;
        g_variant_get(p, "(i&svu)", &id, &ev, &data, &ts);
        menu_event(id, ev);
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "EventGroup")) {
        GVariantIter *it; int id; const char *ev; GVariant *data; guint32 ts;
        g_variant_get(p, "(a(isvu))", &it);
        while (g_variant_iter_next(it, "(i&svu)", &id, &ev, &data, &ts)) { menu_event(id, ev); g_variant_unref(data); }
        g_variant_iter_free(it);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ai)", NULL));
    } else if (!strcmp(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    } else if (!strcmp(method, "AboutToShowGroup")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(aiai)", NULL, NULL));
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
}

static GVariant *menu_get_prop(GDBusConnection *c, const char *s, const char *p, const char *i,
                               const char *name, GError **e, gpointer d)
{
    if (!strcmp(name, "Version")) return g_variant_new_uint32(3);
    if (!strcmp(name, "TextDirection")) return g_variant_new_string("ltr");
    if (!strcmp(name, "Status")) return g_variant_new_string("normal");
    if (!strcmp(name, "IconThemePath")) return g_variant_new_strv(NULL, 0);
    return NULL;
}

static const char MENU_XML[] =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<property name='Version' type='u' access='read'/>"
    "<property name='TextDirection' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconThemePath' type='as' access='read'/>"
    "<method name='GetLayout'><arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='as' direction='in'/>"
    "  <arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/></method>"
    "<method name='GetGroupProperties'><arg type='ai' direction='in'/><arg type='as' direction='in'/><arg type='a(ia{sv})' direction='out'/></method>"
    "<method name='GetProperty'><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
    "<method name='Event'><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='EventGroup'><arg type='a(isvu)' direction='in'/><arg type='ai' direction='out'/></method>"
    "<method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
    "<method name='AboutToShowGroup'><arg type='ai' direction='in'/><arg type='ai' direction='out'/><arg type='ai' direction='out'/></method>"
    "<signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ias)'/></signal>"
    "<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "<signal name='ItemActivationRequested'><arg type='i'/><arg type='u'/></signal>"
    "</interface></node>";

/* ======================================================================== */
/* StatusNotifierItem                                                       */

static const char SNI_XML[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='WindowId' type='i' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconThemePath' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='OverlayIconName' type='s' access='read'/>"
    "<property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
    "<property name='AttentionIconName' type='s' access='read'/>"
    "<property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
    "<property name='AttentionMovieName' type='s' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='Scroll'><arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/></method>"
    "<signal name='NewTitle'/><signal name='NewIcon'/><signal name='NewAttentionIcon'/>"
    "<signal name='NewOverlayIcon'/><signal name='NewToolTip'/>"
    "<signal name='NewStatus'><arg name='status' type='s'/></signal>"
    "</interface></node>";

static char *tooltip_body(void)
{
    if (!M.vols->len) return g_strdup(_("No removable drives"));
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < M.vols->len; i++) {
        Vol *v = M.vols->pdata[i];
        g_string_append_printf(s, "%s%s: %s", i ? "\n" : "", v->label,
                               v->mount ? v->mount : v->locked ? _("locked") : _("not mounted"));
    }
    return g_string_free(s, FALSE);
}

static GVariant *sni_get_prop(GDBusConnection *c, const char *s, const char *p, const char *i,
                              const char *name, GError **e, gpointer d)
{
    if (!strcmp(name, "Category")) return g_variant_new_string("Hardware");
    if (!strcmp(name, "Id")) return g_variant_new_string("ejecta");
    if (!strcmp(name, "Title")) return g_variant_new_string(APP_NAME);
    if (!strcmp(name, "Status")) return g_variant_new_string(M.status);
    if (!strcmp(name, "WindowId")) return g_variant_new_int32(0);
    if (!strcmp(name, "IconName")) return g_variant_new_string("drive-removable-media");
    if (!strcmp(name, "IconThemePath") || !strcmp(name, "OverlayIconName") ||
        !strcmp(name, "AttentionIconName") || !strcmp(name, "AttentionMovieName"))
        return g_variant_new_string("");
    if (g_str_has_suffix(name, "Pixmap")) return g_variant_new("a(iiay)", NULL);
    if (!strcmp(name, "ToolTip")) {
        g_autofree char *body = tooltip_body();
        return g_variant_new("(sa(iiay)ss)", "drive-removable-media", NULL, APP_NAME, body);
    }
    if (!strcmp(name, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (!strcmp(name, "Menu")) return g_variant_new_object_path(MENU_PATH);
    return NULL;
}

static void sni_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                       const char *method, GVariant *p, GDBusMethodInvocation *inv, gpointer d)
{
    int x = -1, y = -1;
    if (!strcmp(method, "Activate") || !strcmp(method, "SecondaryActivate")) {
        g_variant_get(p, "(ii)", &x, &y);
        if (!strcmp(method, "Activate")) panel_toggle(x, y);
        else if (M.vols->len == 1) act_mount(M.vols->pdata[0], TRUE); /* middle click: open */
        else panel_toggle(x, y);
    } else if (!strcmp(method, "ContextMenu")) {
        /* Trays draw the menu from dbusmenu; should one call this instead, show the panel. */
        g_variant_get(p, "(ii)", &x, &y);
        panel_toggle(x, y);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static void sni_emit(const char *signal, GVariant *args)
{
    if (M.session && M.sni_name)
        g_dbus_connection_emit_signal(M.session, NULL, SNI_PATH, "org.kde.StatusNotifierItem", signal, args, NULL);
}

static void sni_register(GDBusConnection *c, const char *name, const char *owner, gpointer d)
{
    g_dbus_connection_call(M.session, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                           "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                           g_variant_new("(s)", M.sni_name), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void sni_start(void)
{
    static const GDBusInterfaceVTable sni_vt = { sni_method, sni_get_prop, NULL, {0} };
    static const GDBusInterfaceVTable menu_vt = { menu_method, menu_get_prop, NULL, {0} };
    GDBusNodeInfo *sni = g_dbus_node_info_new_for_xml(SNI_XML, NULL);
    GDBusNodeInfo *menu = g_dbus_node_info_new_for_xml(MENU_XML, NULL);
    g_dbus_connection_register_object(M.session, SNI_PATH, sni->interfaces[0], &sni_vt, NULL, NULL, NULL);
    g_dbus_connection_register_object(M.session, MENU_PATH, menu->interfaces[0], &menu_vt, NULL, NULL, NULL);
    g_dbus_node_info_unref(sni); g_dbus_node_info_unref(menu);
    M.sni_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", getpid());
    g_bus_own_name_on_connection(M.session, M.sni_name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
    /* registers now and again whenever the tray host restarts */
    g_bus_watch_name_on_connection(M.session, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   sni_register, NULL, NULL, NULL);
}

/* ======================================================================== */
/* refresh and change detection                                             */

static void update_all(void)
{
    const char *st = (M.vols->len || M.always_icon) ? "Active" : "Passive";
    if (M.status != st) { M.status = st; sni_emit("NewStatus", g_variant_new("(s)", st)); }
    sni_emit("NewToolTip", NULL);
    menu_build();
    M.menu_rev++;
    if (M.session)
        g_dbus_connection_emit_signal(M.session, NULL, MENU_PATH, "com.canonical.dbusmenu", "LayoutUpdated",
                                      g_variant_new("(ui)", M.menu_rev, 0), NULL);
    if (M.win && gtk_widget_get_visible(GTK_WIDGET(M.win))) {
        panel_fill();
        GtkWidget *all = g_object_get_data(G_OBJECT(M.win), "eject-all");
        gtk_widget_set_visible(all, M.vols->len > 0);
    }
}

static void notify_inserted(Vol *v)
{
    g_autofree char *sz = human_size(v->size);
    GString *body = g_string_new(NULL);
    if (v->drive_name && g_strcmp0(v->drive_name, v->label)) g_string_append_printf(body, "%s · ", v->drive_name);
    g_string_append(body, sz);
    if (v->locked) g_string_append_printf(body, " · %s", _("encrypted"));
    else if (v->fstype) g_string_append_printf(body, " · %s", v->fstype);
    g_string_append_printf(body, "\n%s", _("Mount it?"));
    g_autofree char *sum = g_strdup_printf(_("Connected: %s"), v->label);
    const char *acts[] = { "mount", _("Mount"), "mount-open", _("Mount & open"), NULL };
    const char *acts_locked[] = { "mount", _("Unlock…"), NULL };
    notify(N_INSERT, v->path, 0, v->icon, sum, body->str, 0,
           (const char *const *)(v->locked ? acts_locked : acts));
    g_string_free(body, TRUE);
}

static void notify_mounted(Vol *v)
{
    g_autofree char *sum = g_strdup_printf(_("Mounted: %s"), v->label);
    g_autofree char *body = g_strdup_printf("%s\n%s", v->mount, _("Open it in the file manager?"));
    const char *acts[] = { "open", _("Open"), NULL };
    notify(N_OPEN, v->path, 0, v->icon, sum, body, 12000, (const char *const *)acts);
}

static gboolean refresh(gpointer d)
{
    M.refresh_id = 0;
    GPtrArray *nv = collect_vols();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < nv->len; i++) {
        Vol *v = nv->pdata[i];
        g_hash_table_add(seen, v->path);
        gboolean is_new = !g_hash_table_contains(M.known, v->path);
        gboolean was_mounted = g_hash_table_contains(M.mounted, v->path);
        if (M.baseline && is_new && v->removable && !v->mount && M.notify_insert)
            notify_inserted(v);
        if (M.baseline && v->mount && !was_mounted) {
            notify_close_for(v->path);
            /* skip the question when the user asked to open it explicitly */
            if (!g_hash_table_remove(M.no_ask, v->path) && M.ask_open) notify_mounted(v);
        }
        if (!v->mount && was_mounted) notify_close_for(v->path);
    }
    /* forget what has disappeared */
    GHashTableIter it; gpointer k;
    g_hash_table_iter_init(&it, M.known);
    while (g_hash_table_iter_next(&it, &k, NULL))
        if (!g_hash_table_contains(seen, k)) {
            notify_close_for(k);
            g_hash_table_remove(M.errors, k);
            g_hash_table_remove(M.expanded, k);
            g_hash_table_remove(M.no_ask, k);
            if (!g_strcmp0(M.unlock_target, k)) g_clear_pointer(&M.unlock_target, g_free);
            g_hash_table_iter_remove(&it);
        }
    g_hash_table_remove_all(M.mounted);
    for (guint i = 0; i < nv->len; i++) {
        Vol *v = nv->pdata[i];
        g_hash_table_add(M.known, g_strdup(v->path));
        if (v->mount) g_hash_table_add(M.mounted, g_strdup(v->path));
    }
    g_hash_table_unref(seen);
    if (M.vols) g_ptr_array_unref(M.vols);
    M.vols = nv;
    M.baseline = TRUE;
    update_all();
    return G_SOURCE_REMOVE;
}

static void schedule_refresh(void)
{
    if (!M.refresh_id) M.refresh_id = g_timeout_add(200, refresh, NULL);
}

static void on_ud_changed(void) { schedule_refresh(); }

/* ======================================================================== */
/* actions                                                                  */

static void do_action(int action, const char *path)
{
    Vol *v = vol_find(path);
    switch (action) {
    case A_OPEN:    if (v) act_mount(v, TRUE); break;
    case A_MOUNT:   if (v) act_mount(v, FALSE); break;
    case A_UNMOUNT: if (v) act_unmount(v); break;
    case A_EJECT:   if (v) act_eject(v); break;
    case A_UNLOCK:
        if (v) { g_free(M.unlock_target); M.unlock_target = g_strdup(v->path); panel_show(-1, -1); }
        break;
    case A_COPY:
        if (v && v->mount) gdk_clipboard_set_text(gdk_display_get_clipboard(gdk_display_get_default()), v->mount);
        break;
    case A_DETAILS:
        if (v) g_hash_table_add(M.expanded, g_strdup(v->path));
        panel_show(-1, -1);
        break;
    case A_MOUNT_ALL:
        for (guint i = 0; i < M.vols->len; i++) { Vol *x = M.vols->pdata[i]; if (!x->mount && !x->locked) act_mount(x, FALSE); }
        break;
    case A_UNMOUNT_ALL:
        for (guint i = 0; i < M.vols->len; i++) { Vol *x = M.vols->pdata[i]; if (x->mount) act_unmount(x); }
        break;
    case A_EJECT_ALL: {
        /* a single eject per physical device */
        GHashTable *done = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint i = 0; i < M.vols->len; i++) {
            Vol *x = M.vols->pdata[i];
            if (!x->removable || (!x->drive && !x->loop)) continue;
            const char *dev = x->drive ? x->drive : x->loop;
            if (!g_hash_table_add(done, (gpointer)dev)) continue;
            act_eject(x);
        }
        g_hash_table_unref(done);
        break;
    }
    case A_PANEL: panel_show(-1, -1); break;
    case A_QUIT:  g_application_quit(G_APPLICATION(M.app)); break;
    case A_T_NOTIFY:   M.notify_insert = !M.notify_insert; config_save(); update_all(); break;
    case A_T_ASKOPEN:  M.ask_open = !M.ask_open; config_save(); update_all(); break;
    case A_T_INTERNAL: M.show_internal = !M.show_internal; config_save(); schedule_refresh(); break;
    case A_T_ICON:     M.always_icon = !M.always_icon; config_save(); update_all(); break;
    case A_KILL_ALL:   if (v) kill_for_vol(v, 0); break;
    case A_REFRESH:    schedule_refresh(); break;
    }
}

static void on_notif_signal(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                            const char *signal, GVariant *p, gpointer d)
{
    guint32 id;
    if (!strcmp(signal, "ActionInvoked")) {
        const char *key;
        g_variant_get(p, "(u&s)", &id, &key);
        NCtx *cx = g_hash_table_lookup(M.notifs, GUINT_TO_POINTER(id));
        if (!cx) return;
        Vol *v = vol_find(cx->path);
        if (v) {
            if (!strcmp(key, "mount")) act_mount(v, FALSE);
            else if (!strcmp(key, "mount-open") || !strcmp(key, "open")) act_mount(v, TRUE);
            else if (!strcmp(key, "kill")) kill_for_vol(v, cx->retry);
            else if (!strcmp(key, "retry")) do_action(cx->retry, v->path);
            else if (!strcmp(key, "show")) { g_hash_table_add(M.expanded, g_strdup(v->path)); panel_show(-1, -1); }
        }
        g_hash_table_remove(M.notifs, GUINT_TO_POINTER(id));
    } else if (!strcmp(signal, "NotificationClosed")) {
        guint32 reason;
        g_variant_get(p, "(uu)", &id, &reason);
        g_hash_table_remove(M.notifs, GUINT_TO_POINTER(id));
    }
}

/* ======================================================================== */
/* startup                                                                  */

static void on_startup(GApplication *app, gpointer d)
{
    GError *e = NULL;
    M.known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    M.mounted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    M.busy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    M.no_ask = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    M.expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    M.errors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    M.notifs = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)ncx_free);
    M.vols = g_ptr_array_new_with_free_func((GDestroyNotify)vol_free);
    M.status = "Passive";
    config_load();
    css_load();
    g_autofree char *cp = css_colors_path();
    GFile *cf = g_file_new_for_path(cp);
    M.css_mon = g_file_monitor_file(cf, G_FILE_MONITOR_NONE, NULL, NULL);
    if (M.css_mon) g_signal_connect(M.css_mon, "changed", G_CALLBACK(on_css_changed), NULL);
    g_object_unref(cf);

    M.session = g_object_ref(g_application_get_dbus_connection(app));
    M.system = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
    if (!M.system) g_error("no system bus: %s", e->message);
    M.ud = g_dbus_object_manager_client_new_sync(M.system, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                 UD_NAME, "/org/freedesktop/UDisks2", NULL, NULL, NULL, NULL, &e);
    if (!M.ud) { g_warning("UDisks2 is not available: %s", e->message); g_clear_error(&e); }
    else {
        g_signal_connect_swapped(M.ud, "object-added", G_CALLBACK(on_ud_changed), NULL);
        g_signal_connect_swapped(M.ud, "object-removed", G_CALLBACK(on_ud_changed), NULL);
        g_signal_connect_swapped(M.ud, "interface-added", G_CALLBACK(on_ud_changed), NULL);
        g_signal_connect_swapped(M.ud, "interface-removed", G_CALLBACK(on_ud_changed), NULL);
        g_signal_connect_swapped(M.ud, "interface-proxy-properties-changed", G_CALLBACK(on_ud_changed), NULL);
    }
    g_dbus_connection_signal_subscribe(M.session, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
                                       NULL, "/org/freedesktop/Notifications", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_notif_signal, NULL, NULL);
    menu_build();
    sni_start();
    refresh(NULL);
    g_application_hold(app);
}

static void on_activate(GApplication *app, gpointer d)
{
    static gboolean first = TRUE;
    if (first) { first = FALSE; return; } /* initial launch: just the tray icon */
    panel_toggle(-1, -1);                 /* running `ejecta` again toggles the panel */
}

static int on_local_options(GApplication *app, GVariantDict *opts, gpointer d)
{
    if (g_variant_dict_contains(opts, "version")) {
        printf("ejecta %s\n", VERSION);
        return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    const char *localedir = g_getenv("EJECTA_LOCALEDIR");
    bindtextdomain(GETTEXT_PACKAGE, localedir ? localedir : LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    M.app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_application_add_main_option(G_APPLICATION(M.app), "version", 'v', G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, _("Print the version and exit"), NULL);
    g_application_set_option_context_summary(G_APPLICATION(M.app),
        _("Removable drives in the system tray. Run it again to toggle the panel."));
    g_signal_connect(M.app, "handle-local-options", G_CALLBACK(on_local_options), NULL);
    g_signal_connect(M.app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(M.app, "activate", G_CALLBACK(on_activate), NULL);
    int r = g_application_run(G_APPLICATION(M.app), argc, argv);
    g_object_unref(M.app);
    return r;
}
