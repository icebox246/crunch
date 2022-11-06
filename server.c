#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <arpa/inet.h>
#include <assert.h>
#include <byteswap.h>
#include <ctype.h>
#include <net/if.h>
#include <sha1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* SETTINGS */
#define INTERFACE "wlo1"
#define PORT 42142
#define USE_QRENCODE 1

#define LOG_EVENTS 0
/* END OF SETTINGS */

#define CMD_DIRK 0x4449524b
#define CMD_MOUS 0x4d4f5553
#define CMD_BUTT 0x42555454

/* WebSocket land */

typedef struct {
    uint8_t fin, opcode;
    size_t payload_len;
#define PAYLOAD_CAPACITY 1024
    char payload[PAYLOAD_CAPACITY];
} UnpackedFrame;

int hexdigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
int parse_nhex(char* inp, int len) {
    int i, o = 0;

    for (i = 0; i < len; i++) {
        o <<= 4;
        o += hexdigit(inp[i]);
    }

    return o;
}

void send_x_key(Display* display, int key, int press) {
    XKeyEvent ev;

    Window root = XDefaultRootWindow(display);
    Window win;
    int revert;
    XGetInputFocus(display, &win, &revert);

    ev.display = display;
    ev.root = root;
    ev.window = win;
    ev.subwindow = None;
    ev.time = CurrentTime;
    ev.x = 1;
    ev.y = 1;
    ev.x_root = 1;
    ev.y_root = 1;
    ev.same_screen = True;
    ev.keycode = XKeysymToKeycode(display, key);
    ev.state = 0;  // modifiers
    if (press)
        ev.type = KeyPress;
    else
        ev.type = KeyRelease;

    XSendEvent(display, ev.window, True, KeyPressMask, (XEvent*)&ev);
    XFlush(display);

#if LOG_EVENTS
    printf("Key send!\n");
#endif
}

void send_x_mouse(Display* display, float xf, float yf) {
    XMotionEvent ev;
    int x, y, w, h;

    Window root = XDefaultRootWindow(display);
    int screen = XDefaultScreen(display);
    w = DisplayWidth(display, screen);
    h = DisplayHeight(display, screen);

    x = w * xf;
    y = h * yf;

    XWarpPointer(display, None, root, 0, 0, 0, 0, x, y);
    XFlush(display);

#if LOG_EVENTS
    printf("Motion send!\n");
#endif
}

void send_x_button(Display* display, int butt, int press) {
    XButtonEvent ev;

    XQueryPointer(display, XRootWindow(display, XDefaultScreen(display)),
                  &ev.root, &ev.window, &ev.x_root, &ev.y_root, &ev.x, &ev.y,
                  &ev.state);

    ev.display = display;
    ev.subwindow = ev.window;
    ev.time = CurrentTime;
    ev.same_screen = True;
    ev.button = butt;
    if (press) {
        ev.type = ButtonPress;
    } else {
        ev.state = 0x100;
        ev.type = ButtonRelease;
    }

    while (ev.subwindow) {
        ev.window = ev.subwindow;
        XQueryPointer(display, ev.window, &ev.root, &ev.subwindow, &ev.x_root,
                      &ev.y_root, &ev.x, &ev.y, &ev.state);
    }

    XSendEvent(display, PointerWindow, True, ButtonPressMask, (XEvent*)&ev);
    XFlush(display);

#if LOG_EVENTS
    printf("Button send!\n");
#endif
}

void send_ws_close(int fd) {
    static const char* msg = "\x88\x00";
    const int msg_len = 2;
    send(fd, msg, msg_len, 0);
}

int parse_payload(char* payload, int payload_len, Display* display) {
    if (payload_len < 4) return -1;
    int cmd_len = 4 + 1;
    int cmd = ntohl(*(int*)payload);

    switch (cmd) {
        case CMD_DIRK: {
            /* direction key commands */
            cmd_len += 1;
            char arg = payload[4];

            switch (arg) {
                case 'r':
                    send_x_key(display, XK_Right, 1);
                    send_x_key(display, XK_Right, 0);
                    break;
                case 'l':
                    send_x_key(display, XK_Left, 1);
                    send_x_key(display, XK_Left, 0);
                    break;
                case 'u':
                    send_x_key(display, XK_Up, 1);
                    send_x_key(display, XK_Up, 0);
                    break;
                case 'd':
                    send_x_key(display, XK_Down, 1);
                    send_x_key(display, XK_Down, 0);
                    break;
                default:
                    printf("Unknown argument `%c` in PRES command\n", arg);
                    return -1;
                    break;
            }
        } break;
        case CMD_MOUS: {
            /* mouse motion commands */
            cmd_len += 4;
            int mous_x = parse_nhex(payload + 4, 4);
            cmd_len += 4;
            int mous_y = parse_nhex(payload + 8, 4);

#if LOG_EVENTS
            printf("Got MOUS with x: %d, y: %d\n", mous_x, mous_y);
#endif

            float xf = mous_x / (float)0xffff;
            float yf = mous_y / (float)0xffff;

            send_x_mouse(display, xf, yf);
        } break;
        case CMD_BUTT: {
            /* mouse button commands */
            cmd_len += 1;
            int butt = parse_nhex(payload + 4, 1);

#if LOG_EVENTS
            printf("Got BUTT with button %x\n", butt);
#endif

            switch (butt) {
                case 0:
                    // left
                    send_x_button(display, Button1, 1);
                    send_x_button(display, Button1, 0);
                    break;
                case 1:
                    // right
                    send_x_button(display, Button3, 1);
                    send_x_button(display, Button3, 0);
                    break;
                case 2:
                    // middle
                    send_x_button(display, Button2, 1);
                    send_x_button(display, Button2, 0);
                    break;
            }
        } break;
        default:
            printf("Unknown command received %08x\n", cmd);
            return -1;
    }

    if (payload[cmd_len - 1] != '\n') {
        printf("Received command not terminated with '\\n'!\n");
        return -1;
    }

    if (payload_len - cmd_len > 0)
        return parse_payload(payload + cmd_len, payload_len - cmd_len, display);

    return 0;
}

int recv_frame(int fd, UnpackedFrame* frame) {
    uint8_t flags, mask_b;
    int mask;
    size_t payload_len;
    size_t padded_payload_len;
    if (recv(fd, &flags, 1, 0) <= 0) {
        return -1;
    }
    frame->fin = !!(flags & 0x80);
    frame->opcode = flags & 0xf;

    recv(fd, &mask_b, 1, 0);
    payload_len = mask_b & 0x7f;
    mask_b >>= 7;

    if (payload_len == 126) {
        payload_len = 0;
        recv(fd, &payload_len, 2, 0);
        payload_len = ntohs(payload_len);
    } else if (payload_len == 127) {
        payload_len = 0;
        recv(fd, &payload_len, 8, 0);
#if __BYTE_ORDER == __LITTLE_ENDIAN
        payload_len = bswap_64(payload_len);
#endif
    }

    frame->payload_len = payload_len;

    if (payload_len > PAYLOAD_CAPACITY) {
        printf("Payload size to big, sorry!\n");
        return -1;
    }

    mask = 0;
    if (mask_b) {
        recv(fd, &mask, 4, 0);
    }

    padded_payload_len = ((payload_len >> 2) + 3) << 2;

    recv(fd, frame->payload, payload_len, 0);

    if (mask_b) {
        int i;
        for (i = 0; i < padded_payload_len; i++) {
            ((int*)frame->payload)[i] ^= mask;
        }
        for (i = payload_len; i < (padded_payload_len); i++)
            frame->payload[i] = '\0';
    }

    return 0;
}

void handle_ws_client(int client_sock) {
    /* open connection to X */
    Display* display = XOpenDisplay(0);

    UnpackedFrame frame = {0};
    printf("WS connection started\n");

    while (recv_frame(client_sock, &frame) >= 0) {
        /* parse message */

        if (frame.opcode == 0x1) {
            /* printf("\nMessage (%d):\n%*s", (int)frame.payload_len, */
            /*        (int)frame.payload_len, frame.payload); */

            if (parse_payload(frame.payload, frame.payload_len, display)) {
                printf("Error during payload parsing!\n");
                break;
            }
        } else if (frame.opcode == 0x8) {
            printf("Ending gracefully!\n");
            break;
        } else {
            printf("Got opcode: 0x%x\n", frame.opcode);
        }
    }

    send_ws_close(client_sock);

    XCloseDisplay(display);

    printf("WS connection ended\n");
}

#define HTTP_VERSION "HTTP/1.1"
#define STATUS_405 "405 Method Not Allowed"
#define STATUS_404 "404 Not Found"
#define STATUS_200 "200 OK"
#define STATUS_101 "101 Switching Protocols"
#define STANDARD_HEADERS \
    "Server: Crunch\r\n" \
    "Access-Control-Allow-Origin: *\r\n"

#define BASE64_CHARS \
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

/* HTTP land */

void base64_encode(uint8_t* input, int n, char* output) {
    int i, j, v, p;
    for (i = 0, j = 0; i + 2 < n; i += 3, j += 4) {
        v = (input[i] << 16) | (input[i + 1] << 8) | (input[i + 2]);
        output[j + 3] = BASE64_CHARS[v & 0x3f];
        v >>= 6;
        output[j + 2] = BASE64_CHARS[v & 0x3f];
        v >>= 6;
        output[j + 1] = BASE64_CHARS[v & 0x3f];
        v >>= 6;
        output[j] = BASE64_CHARS[v & 0x3f];
    }
    if (i == n) return;

    v = 0;

    p = 0;

    if (i < n) v |= input[i] << 16;

    if (i + 1 < n) {
        v |= input[i + 1] << 8;
        p = 1;
    } else
        p = 2;

    output[j + 3] = p < 1 ? (BASE64_CHARS[v & 0x3f]) : '=';
    v >>= 6;
    output[j + 2] = p < 2 ? BASE64_CHARS[v & 0x3f] : '=';
    v >>= 6;
    output[j + 1] = p < 3 ? (BASE64_CHARS[v & 0x3f]) : '=';
    v >>= 6;
    output[j] = p < 4 ? (BASE64_CHARS[v & 0x3f]) : '=';
}

void compute_answer(char* key, char* answer) {
    uint8_t result[SHA1_DIGEST_LENGTH];
    char buffer[256] = {0};
    int key_len = strlen(key);
    memcpy(buffer, key, key_len);
    strcpy(buffer + key_len, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, (uint8_t*)buffer, strlen(buffer));
    SHA1Final(result, &ctx);

    /* printf("digest: "); */
    /* for (int n = 0; n < SHA1_DIGEST_LENGTH; n++) { */
    /*     printf("%02x", result[n]); */
    /* } */
    /* printf("\n"); */

    base64_encode(result, SHA1_DIGEST_LENGTH, answer);
}

void send_websocket_handshake(int fd, char* answer) {
    char msg[256] = {0};
    int msg_len = sprintf(msg,
                          HTTP_VERSION " " STATUS_101
                                       "\r\n"
                                       "Upgrade: websocket\r\n"
                                       "Connection: Upgrade\r\n"
                                       "Sec-WebSocket-Accept: %s\r\n"
                                       "\r\n",
                          answer);

    send(fd, msg, msg_len, 0);
}

void send_unsupported_method_error(int fd) {
    static const char* msg =
        HTTP_VERSION STATUS_405 "\r\n" STANDARD_HEADERS
                                "Content-Type: text/html\r\n"
                                "Content-Length: 22\r\n"
                                "\r\n" STATUS_405;
    const int msg_len = strlen(msg);

    send(fd, msg, msg_len, 0);
}

void send_not_found(int fd) {
    static const char* msg =
        HTTP_VERSION STATUS_404 "\r\n" STANDARD_HEADERS
                                "Content-Type: text/html\r\n"
                                "Content-Length: 13\r\n"
                                "\r\n" STATUS_404;
    const int msg_len = strlen(msg);

    send(fd, msg, msg_len, 0);
}

void send_html(int fd, const char* html) {
    int len = strlen(html);

    static const char* header =
        HTTP_VERSION " " STATUS_200 "\r\n" STANDARD_HEADERS
                     "Content-Type: text/html\r\nContent-Length: %d\r\n\r\n%s";
    const int header_len = strlen(header);

    char msg[header_len + len + 1];

    sprintf(msg, header, len, html);

    send(fd, msg, header_len + len, 0);
}

void send_svg(int fd, const char* svg) {
    int len = strlen(svg);

    static const char* header = HTTP_VERSION
        " " STATUS_200 "\r\n" STANDARD_HEADERS
        "Content-Type: image/svg+xml\r\nContent-Length: %d\r\n\r\n%s";
    const int header_len = strlen(header);

    char msg[header_len + len + 1];

    sprintf(msg, header, len, svg);

    send(fd, msg, header_len + len, 0);
}

void send_hello(int fd) {
    static const char* html =
        "<h1>Hello!</h1>"
        "<p>Welcome to Crunch HTTP server. :)</p>";

    send_html(fd, html);
}

char* parse_word(char* inp, char* res) {
    while (isspace(*inp)) inp++;
    while (!isspace(*inp) && *inp) {
        if (res) *res++ = *inp;
        inp++;
    }
    return inp + (*inp != 0);
}

char* parse_line(char* inp, char* res) {
    while (*inp != '\n' && *inp) {
        if (res) *res++ = *inp;
        inp++;
    }
    return inp + (*inp != 0);
}

char* parse_header(char* inp, char* hres, char* vres) {
    inp = parse_word(inp, hres);
    inp = parse_line(inp, vres);

    // Remove ':'
    int hlen = strlen(hres);
    hres[hlen - 1] = '\0';

    // Remove "\r\n"
    int vlen = strlen(vres);
    vres[vlen - 1] = '\0';

    return inp;
}

char* load_file(const char* name) {
    char* buff;
    int size;
    FILE* f = fopen(name, "rb");

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);

    buff = malloc(size + 1);
    buff[size] = '\0';

    fread(buff, size, 1, f);

    fclose(f);

    return buff;
}

void handle_http_client(int client_sock) {
    char buffer[1024] = {0};
    char* cursor;
    while (recv(client_sock, buffer, sizeof(buffer), 0) >= 0) {
        cursor = buffer;

        char method[16] = {0};
        cursor = parse_word(cursor, method);

        char url[256] = {0};
        cursor = parse_word(cursor, url);

        // version
        cursor = parse_word(cursor, 0);

        if (strcmp(method, "GET") == 0) {
            printf("%s %s\n", method, url);

            if (strcmp(url, "/") == 0) {
                char* index_page = load_file("www/index.html");
                send_html(client_sock, index_page);
                free(index_page);
            } else if (strcmp(url, "/icon.svg") == 0) {
                char* icon_svg = load_file("www/icon.svg");
                send_svg(client_sock, icon_svg);
                free(icon_svg);
            } else if (strcmp(url, "/ws") == 0) {
                // do websockety magic
                int is_websocket = 0;
                char websocket_key[256] = {0};
                while (*cursor) {
                    char header[64] = {0};
                    char value[256] = {0};
                    cursor = parse_header(cursor, header, value);
                    if (strlen(header) == 0) break;

                    if (strcmp(header, "Upgrade") == 0 &&
                        strcmp(value, "websocket") == 0)
                        is_websocket |= 0b01;

                    if (strcmp(header, "Connection") == 0 &&
                        strcmp(value, "Upgrade") == 0)
                        is_websocket |= 0b10;

                    if (strcmp(header, "Sec-WebSocket-Key") == 0)
                        strcpy(websocket_key, value);

                    /* printf("%s -> \"%s\"\n", header, value); */
                }

                if (is_websocket == 0b11) {
                    char answer[64] = {0};
                    compute_answer(websocket_key, answer);
                    send_websocket_handshake(client_sock, answer);
                    handle_ws_client(client_sock);
                } else {
                    send_html(client_sock, "This is the WebSocket endpoint.");
                }
            } else {
                send_not_found(client_sock);
            }
        } else {
            send_unsupported_method_error(client_sock);
        }

        memset(buffer, 0, sizeof(buffer));
    }

    close(client_sock);
}

/* TCP land */

struct in_addr get_my_ip() {
    int fd;
    struct ifreq ifr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);

    ifr.ifr_addr.sa_family = AF_INET;

    snprintf(ifr.ifr_name, IFNAMSIZ, INTERFACE);

    ioctl(fd, SIOCGIFADDR, &ifr);

    close(fd);

    return ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr;
}

int main(void) {
    printf("Hello, modern World!\n");

    int server_sock, client_sock;
    int server_running;
    struct sockaddr_in server_addr;

    /* create TCP socket */
    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    /* make TCP socket resue port (makes restarts faster) */
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEPORT, &(int){1},
                   sizeof(int))) {
        perror("setsockopt(SO_REUSEPORT)");
        exit(1);
    }

    server_addr.sin_addr = get_my_ip();
    server_addr.sin_port = htons(PORT);
    server_addr.sin_family = AF_INET;

    /* bind TCP socket to a local address */
    if (bind(server_sock, (struct sockaddr*)&server_addr,
             sizeof(server_addr))) {
        perror("bind");
        exit(1);
    }

    /* listen on TCP socket */
    if (listen(server_sock, 1)) {
        perror("listen");
        exit(1);
    }

    printf(
        "\n\x1b[33m _______________________________________________\n |\n |\n");
    printf(" |   \x1b[0mListening on \x1b[32mhttp://%s:%d/\n",
           inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));
    printf(
        "\x1b[33m |\n "
        "|______________________________________________\n\x1b[0m\n");

#if USE_QRENCODE

    int pid;

    if ((pid = fork()) == 0) {
        char url[256] = {0};
        sprintf(url, "http://%s:%d/\n", inet_ntoa(server_addr.sin_addr),
                ntohs(server_addr.sin_port));

        const char* args[] = {"qrencode", "-t", "ansiutf8", url,
                              "-m",       "4",  NULL};

        execvp(args[0], (char* const*)args);
    }
    waitpid(pid, NULL, 0);
    printf("\n");

#endif

    server_running = 1;

    while (server_running) {
        /* accept incomming connections */
        client_sock = accept(server_sock, 0, 0);

        if (!fork()) {
            /* handle clients as HTTP clients in a new process */
            handle_http_client(client_sock);
            exit(0);
        }
    }

    close(server_sock);

    return 0;
}
