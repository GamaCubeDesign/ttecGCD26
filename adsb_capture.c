/*
 * adsb_capture.c
 * ---------------
 * Módulo único: liga o dump1090-fa, conecta na porta SBS-1/BaseStation,
 * carimba cada mensagem recebida com o horário de chegada e grava tudo
 * em um arquivo NDJSON (uma linha = uma mensagem = um evento).
 *
 * Atende HLR-ADS-07: timestamp por mensagem com resolução suficiente
 * para reconstrução de trajetória e ordenação temporal.
 *
 * Pré-condição: o relógio de sistema da Raspberry Pi já deve estar
 * ajustado (ex. via sincronização com a ground station) ANTES de
 * chamar main(). Este código confia direto no CLOCK_REALTIME.
 *
 * Compilar:
 *   gcc -O2 -o adsb_capture adsb_capture.c
 *
 * Uso:
 *   ./adsb_capture
 *   (edite as constantes de configuração logo abaixo antes de compilar,
 *    ou adapte para ler de argv/arquivo de config)
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ======================= CONFIGURAÇÃO ======================= */

#define DUMP1090_BINARY_PATH   "/home/pedro/dump1090/dump1090"
#define JSON_SNAPSHOT_DIR      "/home/pedro/adsb/json"
#define OUTPUT_NDJSON_PATH     "/home/pedro/adsb/eventos.ndjson"
#define DEVICE_INDEX            0
#define GAIN                    "-10"
#define PPM                     0
#define WRITE_JSON_EVERY_SEC    1
#define SBS_PORT                30003
#define BIND_ADDRESS            "127.0.0.1"

/* ======================= LANÇAMENTO DO DUMP1090-FA ======================= */

/*
 * Inicia o dump1090-fa como processo filho via fork+execv (sem shell,
 * mais leve que system() — importante na Pi Zero 2W).
 *
 * Flags usadas eliminam todo overhead que não seja "RTL-SDR -> decodifica":
 * sem modo interativo (ncurses), sem stats acumuladas, sem servidores de
 * rede desnecessários. A única porta de rede mantida é a SBS (necessária
 * para o log por mensagem), presa em loopback.
 *
 * Retorna o PID do processo filho, ou -1 em erro.
 */
static pid_t start_dump1090(void)
{
    char buf_device[16], buf_ppm[16], buf_every[16], buf_sbs_port[16];
    snprintf(buf_device, sizeof(buf_device), "%d", DEVICE_INDEX);
    snprintf(buf_ppm, sizeof(buf_ppm), "%d", PPM);
    snprintf(buf_every, sizeof(buf_every), "%d", WRITE_JSON_EVERY_SEC);
    snprintf(buf_sbs_port, sizeof(buf_sbs_port), "%d", SBS_PORT);

    char *argv[] = {
        (char *)DUMP1090_BINARY_PATH,
        "--device-index", buf_device,
        "--gain", (char *)GAIN,
        "--ppm", buf_ppm,
        "--write-json", (char *)JSON_SNAPSHOT_DIR,
        "--write-json-every", buf_every,
        "--json-location-accuracy", "0",
        "--net",
        //"--net-only",
        "--net-bind-address", (char *)BIND_ADDRESS,
        "--net-ro-port", "0",
        "--net-sbs-port", buf_sbs_port,
        "--net-bi-port", "0",
        "--net-bo-port", "0",
        "--net-ri-port", "0",
        "--quiet",
        NULL
    };

    pid_t pid = fork();

    if (pid < 0) {
        return -1; /* fork falhou */
    }

    if (pid == 0) {
        /* processo filho: substitui a imagem direto via execv */
        execv(DUMP1090_BINARY_PATH, argv);
        _exit(127); /* só chega aqui se execv falhar */
    }

    return pid; /* processo pai: retorna sem bloquear */
}

/* Encerra o dump1090-fa de forma limpa (SIGTERM), com fallback SIGKILL
 * se não encerrar dentro de timeout_ms. */
static void stop_dump1090(pid_t pid, int timeout_ms)
{
    if (pid <= 0) return;

    kill(pid, SIGTERM);

    int waited_ms = 0;
    const int step_ms = 50;

    while (waited_ms < timeout_ms) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return; /* encerrou de forma limpa */
        }
        struct timespec req = { .tv_sec = 0, .tv_nsec = step_ms * 1000000L };
        nanosleep(&req, NULL);
        waited_ms += step_ms;
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* ======================= CLIENTE SBS ======================= */

#define SBS_ICAO_LEN      7
#define SBS_CALLSIGN_LEN  9
#define SBS_RAW_LINE_LEN  256
#define SBS_MAX_FIELDS    24
#define LINE_BUF_CAP      2048

typedef struct {
    struct timespec rx_time;
    uint64_t        rx_epoch_ns;   /* timestamp de recepção — chave do HLR-ADS-07 */

    int    transmission_type;
    char   icao[SBS_ICAO_LEN];
    char   callsign[SBS_CALLSIGN_LEN];

    int    has_altitude;      int altitude_ft;
    int    has_ground_speed;  double ground_speed_kt;
    int    has_track;         double track_deg;
    int    has_position;      double latitude, longitude;
    int    has_vertical_rate; int vertical_rate_fpm;
    int    has_squawk;        char squawk[5];
    int    on_ground;         /* 1 solo, 0 ar, -1 desconhecido */
} sbs_message_t;

/* Buffer de linha: assume UMA conexão por processo (cenário do payload). */
static char   g_line_buf[LINE_BUF_CAP];
static size_t g_line_buf_len = 0;

static int sbs_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    g_line_buf_len = 0;
    return fd;
}

static int read_line(int fd, char *out_line, size_t out_cap)
{
    for (;;) {
        for (size_t i = 0; i < g_line_buf_len; i++) {
            if (g_line_buf[i] == '\n') {
                size_t len = i;
                if (len > 0 && g_line_buf[len - 1] == '\r') len--;
                if (len >= out_cap) len = out_cap - 1;

                memcpy(out_line, g_line_buf, len);
                out_line[len] = '\0';

                size_t consumed = i + 1;
                memmove(g_line_buf, g_line_buf + consumed, g_line_buf_len - consumed);
                g_line_buf_len -= consumed;

                return (int)len;
            }
        }

        if (g_line_buf_len >= LINE_BUF_CAP) {
            g_line_buf_len = 0; /* linha maior que o buffer: descarta */
        }

        ssize_t n = recv(fd, g_line_buf + g_line_buf_len, LINE_BUF_CAP - g_line_buf_len, 0);
        if (n <= 0) return -1;

        g_line_buf_len += (size_t)n;
    }
}

static int split_csv(char *line, char *fields[], int max_fields)
{
    int count = 0;
    char *p = line;
    fields[count++] = p;

    while (*p && count < max_fields) {
        if (*p == ',') {
            *p = '\0';
            fields[count++] = p + 1;
        }
        p++;
    }
    return count;
}

static const char *field_or_empty(char *fields[], int nfields, int idx)
{
    if (idx < 0 || idx >= nfields) return "";
    return fields[idx];
}

/* Lê e parseia a próxima mensagem MSG (bloqueante). Descarta silenciosamente
 * linhas de outros tipos (ID, AIR, STA, CLK). O timestamp em out->rx_time
 * é capturado logo após a linha completa chegar — instante de referência
 * do HLR-ADS-07. Retorna 0 em sucesso, -1 em erro/conexão encerrada. */
static int sbs_read_message(int fd, sbs_message_t *out)
{
    char raw[SBS_RAW_LINE_LEN];

    for (;;) {
        int len = read_line(fd, raw, sizeof(raw));

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        if (len < 0) return -1;
        if (len == 0) continue;

        char parse_buf[SBS_RAW_LINE_LEN];
        memcpy(parse_buf, raw, (size_t)len + 1);

        char *fields[SBS_MAX_FIELDS];
        int nfields = split_csv(parse_buf, fields, SBS_MAX_FIELDS);

        if (nfields < 1 || strcmp(fields[0], "MSG") != 0) continue;

        memset(out, 0, sizeof(*out));
        out->rx_time     = ts;
        out->rx_epoch_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

        out->transmission_type = atoi(field_or_empty(fields, nfields, 1));

        strncpy(out->icao, field_or_empty(fields, nfields, 4), SBS_ICAO_LEN - 1);
        out->icao[SBS_ICAO_LEN - 1] = '\0';

        strncpy(out->callsign, field_or_empty(fields, nfields, 10), SBS_CALLSIGN_LEN - 1);
        out->callsign[SBS_CALLSIGN_LEN - 1] = '\0';
        for (int i = (int)strlen(out->callsign) - 1; i >= 0 && out->callsign[i] == ' '; i--) {
            out->callsign[i] = '\0';
        }

        const char *alt = field_or_empty(fields, nfields, 11);
        if (alt[0]) { out->has_altitude = 1; out->altitude_ft = atoi(alt); }

        const char *gs = field_or_empty(fields, nfields, 12);
        if (gs[0]) { out->has_ground_speed = 1; out->ground_speed_kt = atof(gs); }

        const char *trk = field_or_empty(fields, nfields, 13);
        if (trk[0]) { out->has_track = 1; out->track_deg = atof(trk); }

        const char *lat = field_or_empty(fields, nfields, 14);
        const char *lon = field_or_empty(fields, nfields, 15);
        if (lat[0] && lon[0]) {
            out->has_position = 1;
            out->latitude  = atof(lat);
            out->longitude = atof(lon);
        }

        const char *vr = field_or_empty(fields, nfields, 16);
        if (vr[0]) { out->has_vertical_rate = 1; out->vertical_rate_fpm = atoi(vr); }

        const char *sq = field_or_empty(fields, nfields, 17);
        if (sq[0]) {
            out->has_squawk = 1;
            strncpy(out->squawk, sq, sizeof(out->squawk) - 1);
            out->squawk[sizeof(out->squawk) - 1] = '\0';
        }

        const char *og = field_or_empty(fields, nfields, 21);
        out->on_ground = (og[0] == '1') ? 1 : (og[0] == '0') ? 0 : -1;

        return 0;
    }
}

/* ======================= SERIALIZAÇÃO NDJSON ======================= */

static int append_fmt(char *buf, size_t buf_len, size_t *pos, const char *fmt, ...)
{
    if (*pos >= buf_len) return -1;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, buf_len - *pos, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= buf_len - *pos) return -1;

    *pos += (size_t)n;
    return 0;
}

static int message_to_ndjson(const sbs_message_t *msg, char *buf, size_t buf_len)
{
    size_t pos = 0;

    if (append_fmt(buf, buf_len, &pos,
            "{\"icao\":\"%s\",\"rx_epoch_ns\":%llu,"
            "\"transmission_type\":%d,\"callsign\":\"%s\"",
            msg->icao, (unsigned long long)msg->rx_epoch_ns,
            msg->transmission_type, msg->callsign) != 0) return -1;

    if (msg->has_altitude &&
        append_fmt(buf, buf_len, &pos, ",\"altitude_ft\":%d", msg->altitude_ft) != 0) return -1;

    if (msg->has_ground_speed &&
        append_fmt(buf, buf_len, &pos, ",\"ground_speed_kt\":%.1f", msg->ground_speed_kt) != 0) return -1;

    if (msg->has_track &&
        append_fmt(buf, buf_len, &pos, ",\"track_deg\":%.1f", msg->track_deg) != 0) return -1;

    if (msg->has_position &&
        append_fmt(buf, buf_len, &pos, ",\"lat\":%.6f,\"lon\":%.6f", msg->latitude, msg->longitude) != 0) return -1;

    if (msg->has_vertical_rate &&
        append_fmt(buf, buf_len, &pos, ",\"vertical_rate_fpm\":%d", msg->vertical_rate_fpm) != 0) return -1;

    if (msg->has_squawk &&
        append_fmt(buf, buf_len, &pos, ",\"squawk\":\"%s\"", msg->squawk) != 0) return -1;

    if (append_fmt(buf, buf_len, &pos, ",\"on_ground\":%d}", msg->on_ground) != 0) return -1;

    return (int)pos;
}

/* ======================= MAIN ======================= */

int main(void)
{
    pid_t pid = start_dump1090();
    if (pid < 0) {
        fprintf(stderr, "Falha ao iniciar dump1090-fa\n");
        return 1;
    }
    fprintf(stderr, "dump1090-fa iniciado (PID %d)\n", pid);

    /* Espera o listener SBS subir antes de conectar. */
    sleep(2);

    int fd = -1;
    for (int tentativa = 0; tentativa < 10 && fd < 0; tentativa++) {
        fd = sbs_connect(BIND_ADDRESS, SBS_PORT);
        if (fd < 0) {
            struct timespec req = { .tv_sec = 0, .tv_nsec = 200000000L };
            nanosleep(&req, NULL);
        }
    }

    if (fd < 0) {
        fprintf(stderr, "Falha ao conectar na porta SBS\n");
        stop_dump1090(pid, 2000);
        return 1;
    }

    FILE *out = fopen(OUTPUT_NDJSON_PATH, "a");
    if (out == NULL) {
        fprintf(stderr, "Falha ao abrir arquivo de saida\n");
        close(fd);
        stop_dump1090(pid, 2000);
        return 1;
    }

    sbs_message_t msg;
    char line[512];

    /* Loop principal de captura. Ligue a condição de parada ao ciclo
     * de missão (ex. 10 minutos do HLR-ADS-05) — aqui roda indefinidamente. */
    while (sbs_read_message(fd, &msg) == 0) {
        int n = message_to_ndjson(&msg, line, sizeof(line));
        if (n > 0) {
            fputs(line, out);
            fputc('\n', out);
            fflush(out); /* garante persistência mesmo em corte abrupto */
        }
    }

    fclose(out);
    close(fd);
    stop_dump1090(pid, 2000);

    return 0;
}
