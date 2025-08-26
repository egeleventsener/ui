// ui_client.c - GTK3 split view client for existing C server
// Build (Ubuntu):
//   sudo apt install libgtk-3-dev
//   gcc ui_client.c -o client_ui `pkg-config --cflags --libs gtk+-3.0`
// Build (Windows MSYS2 UCRT64):
//   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-gtk3 pkgconf
//   gcc ui_client.c -o client_ui.exe $(pkg-config --cflags --libs gtk+-3.0) -lws2_32
//
// Usage:
//   ./client_ui <server_ip> <port>
//
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define CLOSESOCK closesocket
  static int sock_err(void){ return WSAGetLastError(); }
  static void sleep_ms(unsigned ms){ Sleep(ms); }
#else
  #include <unistd.h>
  #include <arpa/inet.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  typedef int sock_t;
  #define INVALID_SOCKET (-1)
  #define CLOSESOCK close
  static int sock_err(void){ return errno; }
  static void sleep_ms(unsigned ms){ usleep(ms*1000); }
#endif

// Adjust if your server uses a different command for listing
#define SERVER_LS_CMD "sls"   // change to "ls" if needed
#define SERVER_PWD_CMD "pwd"
#define SERVER_CD_CMD  "cd"
#define SERVER_MKDIR_CMD "mkdir"
#define SERVER_RM_CMD "rm"
#define SERVER_RENAME_CMD "rename"
#define SERVER_PUT_CMD "put"  // name then bytes then EOF

typedef struct {
    GtkWidget *tv_server;
    GtkWidget *tv_client;
    GtkWidget *entry_srv_path;
    GtkWidget *entry_cli_path;
    GtkWidget *status;
    GtkWidget *btn_upload;
    GtkListStore *store_srv;
    GtkListStore *store_cli;
    GMutex ui_mutex;

    sock_t sock;
    gchar cwd_local[1024];
} App;

enum { COL_NAME = 0, COL_TYPE, COL_SIZE, N_COLS };

static void status_msg(App *app, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    guint cid = gtk_statusbar_get_context_id(GTK_STATUSBAR(app->status), "main");
    gtk_statusbar_push(GTK_STATUSBAR(app->status), cid, buf);
}

static GtkListStore* make_store(void)
{
    return gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

static void attach_store_to_tree(GtkWidget *tv, GtkListStore *store)
{
    gtk_tree_view_set_model(GTK_TREE_VIEW(tv), GTK_TREE_MODEL(store));
    for (int i=0;i<N_COLS;i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        const char *titles[N_COLS] = {"Name","Type","Size"};
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }
}

static void store_clear(GtkListStore *s){ gtk_list_store_clear(s); }

static void store_add(GtkListStore *s, const char *name, const char *type, const char *size)
{
    GtkTreeIter it;
    gtk_list_store_append(s, &it);
    gtk_list_store_set(s, &it, COL_NAME, name, COL_TYPE, type?type:"", COL_SIZE, size?size:"", -1);
}

/* ---- Networking helpers ---- */
static int sendf(sock_t s, const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (send(s, buf, n, 0) != n) return -1;
    return 0;
}

static int recv_line(sock_t s, char *out, size_t cap, int timeout_ms)
{
    size_t pos = 0;
    long waited = 0;
    while (pos+1 < cap) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r == 1) {
            if (c == '\\n') break;
            out[pos++] = c;
        } else if (r == 0) {
            break; // connection closed
        } else {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) { sleep_ms(5); waited+=5; if (waited>=timeout_ms) break; continue; }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(5000); waited+=5; if (waited>=timeout_ms) break; continue; }
#endif
            return -1;
        }
    }
    out[pos] = 0;
    return (int)pos;
}

typedef struct {
    App *app;
} RefreshSrvCtx;

static gboolean ui_update_srv_list(gpointer data)
{
    // data is a gchar** array: lines terminated by NULL
    gpointer *arr = data;
    App *app = arr[0];
    char **lines = (char**)arr[1];

    store_clear(app->store_srv);
    for (char **p = lines; p && *p; ++p) {
        if (**p == 0) continue;
        store_add(app->store_srv, *p, "", "");
    }
    status_msg(app, "Server listed %d items", lines ? g_strv_length(lines) : 0);
    if (lines) g_strfreev(lines);
    g_free(arr);
    return FALSE;
}

static gpointer refresh_server_thread(gpointer user)
{
    App *app = (App*)user;
    // Request listing
    sendf(app->sock, SERVER_LS_CMD "\\n");
    // Read lines until blank read or timeout accumulation
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);

    for (;;) {
        char line[1024];
        int n = recv_line(app->sock, line, sizeof(line), 500);
        if (n <= 0) break;
        g_ptr_array_add(a, g_strdup(line));
        if (a->len > 10000) break; // safety
        // Heuristic: stop if server sends END
        if (g_strcmp0(line, "END") == 0) break;
    }
    g_ptr_array_add(a, NULL);
    gpointer *pack = g_new0(gpointer, 2);
    pack[0] = app;
    pack[1] = a->pdata; // ownership of vector
    // do not free a, transfer its data vector; avoid g_ptr_array_free(a, TRUE)
    g_idle_add(ui_update_srv_list, pack);
    // free the container but not data
    g_ptr_array_free(a, FALSE);
    return NULL;
}

static void refresh_server(App *app)
{
    g_thread_new("srv-refresh", refresh_server_thread, app);
}

static void refresh_client(App *app)
{
    const char *path = gtk_entry_get_text(GTK_ENTRY(app->entry_cli_path));
    if (!path || !*path) path = ".";
    if (chdir(path) != 0) {
        status_msg(app, "chdir('%s') failed: %s", path, g_strerror(errno));
        return;
    }
    getcwd(app->cwd_local, sizeof(app->cwd_local));
    gtk_entry_set_text(GTK_ENTRY(app->entry_cli_path), app->cwd_local);

    store_clear(app->store_cli);
    DIR *d = opendir(".");
    if (!d) { status_msg(app, "opendir failed"); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (stat(e->d_name, &st) == 0) {
            if (S_ISDIR(st.st_mode)) store_add(app->store_cli, e->d_name, "dir", "");
            else {
                char sz[64]; snprintf(sz, sizeof(sz), "%lld", (long long)st.st_size);
                store_add(app->store_cli, e->d_name, "file", sz);
            }
        } else {
            store_add(app->store_cli, e->d_name, "", "");
        }
    }
    closedir(d);
    status_msg(app, "Local listed");
}

/* ---- Upload (PUT) ---- */
static int put_file(sock_t s, const char *local_path, const char *remote_name)
{
    FILE *fp = fopen(local_path, "rb");
    if (!fp) return -1;
    // protocol: PUT name\\n then raw bytes then EOF
    if (sendf(s, SERVER_PUT_CMD " %s\\n", remote_name) != 0) { fclose(fp); return -1; }
    char buf[4096];
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n > 0) {
            if (send(s, buf, (int)n, 0) != (int)n) { fclose(fp); return -1; }
        }
        if (n < sizeof(buf)) {
            if (ferror(fp)) { fclose(fp); return -1; }
            break;
        }
    }
    send(s, "EOF", 3, 0);
    fclose(fp);
    return 0;
}

/* ---- Callbacks ---- */
static void on_srv_refresh(GtkButton *b, gpointer u){ refresh_server((App*)u); }

static void on_cli_refresh(GtkButton *b, gpointer u){ refresh_client((App*)u); }

static void on_srv_cd(GtkButton *b, gpointer u)
{
    App *app = (App*)u;
    const char *p = gtk_entry_get_text(GTK_ENTRY(app->entry_srv_path));
    if (!p || !*p) return;
    sendf(app->sock, SERVER_CD_CMD " %s\\n", p);
    refresh_server(app);
}

static void on_cli_cd(GtkButton *b, gpointer u){ refresh_client((App*)u); }

static void on_upload(GtkButton *b, gpointer u)
{
    App *app = (App*)u;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Select file", GTK_WINDOW(gtk_widget_get_toplevel(b)),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        // derive base name
        const char *base = g_path_get_basename(path);
        if (put_file(app->sock, path, base) == 0) status_msg(app, "Uploaded %s", base);
        else status_msg(app, "Upload failed");
        g_free((gpointer)base);
        g_free(path);
        refresh_server(app);
    }
    gtk_widget_destroy(dlg);
}

/* ---- UI construction (no Glade) ---- */
static GtkWidget* make_toolbar(App *app, gboolean server_side)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (server_side) {
        GtkWidget *btn = gtk_button_new_with_label("Refresh");
        g_signal_connect(btn, "clicked", G_CALLBACK(on_srv_refresh), app);
        gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
        btn = gtk_button_new_with_label("Upload");
        app->btn_upload = btn;
        g_signal_connect(btn, "clicked", G_CALLBACK(on_upload), app);
        gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    } else {
        GtkWidget *btn = gtk_button_new_with_label("Refresh");
        g_signal_connect(btn, "clicked", G_CALLBACK(on_cli_refresh), app);
        gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    }
    return box;
}

static GtkWidget* build_ui(App *app)
{
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Client UI");
    gtk_window_set_default_size(GTK_WINDOW(win), 1000, 600);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(win), paned);

    // Left: Server
    GtkWidget *boxL = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_paned_pack1(GTK_PANED(paned), boxL, TRUE, FALSE);
    GtkWidget *lblL = gtk_label_new("Server");
    gtk_box_pack_start(GTK_BOX(boxL), lblL, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(boxL), make_toolbar(app, TRUE), FALSE, FALSE, 0);

    GtkWidget *swL = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(boxL), swL, TRUE, TRUE, 0);
    app->tv_server = gtk_tree_view_new();
    gtk_container_add(GTK_CONTAINER(swL), app->tv_server);

    GtkWidget *pathRowL = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(boxL), pathRowL, FALSE, FALSE, 0);
    app->entry_srv_path = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry_srv_path), "/ on server");
    gtk_box_pack_start(GTK_BOX(pathRowL), app->entry_srv_path, TRUE, TRUE, 0);
    GtkWidget *btnSrvCd = gtk_button_new_with_label("cd");
    g_signal_connect(btnSrvCd, "clicked", G_CALLBACK(on_srv_cd), app);
    gtk_box_pack_start(GTK_BOX(pathRowL), btnSrvCd, FALSE, FALSE, 0);

    // Right: Client
    GtkWidget *boxR = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_paned_pack2(GTK_PANED(paned), boxR, TRUE, FALSE);
    GtkWidget *lblR = gtk_label_new("Client (Local)");
    gtk_box_pack_start(GTK_BOX(boxR), lblR, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(boxR), make_toolbar(app, FALSE), FALSE, FALSE, 0);

    GtkWidget *swR = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(boxR), swR, TRUE, TRUE, 0);
    app->tv_client = gtk_tree_view_new();
    gtk_container_add(GTK_CONTAINER(swR), app->tv_client);

    GtkWidget *pathRowR = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(boxR), pathRowR, FALSE, FALSE, 0);
    app->entry_cli_path = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->entry_cli_path), ".");
    gtk_box_pack_start(GTK_BOX(pathRowR), app->entry_cli_path, TRUE, TRUE, 0);
    GtkWidget *btnCliCd = gtk_button_new_with_label("cd");
    g_signal_connect(btnCliCd, "clicked", G_CALLBACK(on_cli_cd), app);
    gtk_box_pack_start(GTK_BOX(pathRowR), btnCliCd, FALSE, FALSE, 0);

    // Statusbar
    app->status = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(boxR), app->status, FALSE, FALSE, 0);

    // Stores
    app->store_srv = make_store();
    app->store_cli = make_store();
    attach_store_to_tree(app->tv_server, app->store_srv);
    attach_store_to_tree(app->tv_client, app->store_cli);

    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    return win;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\\n", argv[0]);
        return 1;
    }

    // Connect to server
    sock_t s = INVALID_SOCKET;
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP\\n"); return 1;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { perror("socket"); return 1; }
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("connect"); CLOSESOCK(s); return 1;
    }

    App app = {0};
    app.sock = s;
    getcwd(app.cwd_local, sizeof(app.cwd_local));

    GtkWidget *win = build_ui(&app);
    gtk_widget_show_all(win);

    // Initial lists
    refresh_server(&app);
    gtk_entry_set_text(GTK_ENTRY(app.entry_cli_path), app.cwd_local);
    refresh_client(&app);

    gtk_main();

    CLOSESOCK(app.sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
