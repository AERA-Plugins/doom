/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DOOM_IMPLEMENT_PRINT
#define DOOM_IMPLEMENT_MALLOC
#define DOOM_IMPLEMENT_FILE_IO
#define DOOM_IMPLEMENT_GETTIME
#define DOOM_IMPLEMENT_EXIT
#define DOOM_IMPLEMENT_GETENV
#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"

#define AERA_MAGIC 0x414d4f44U
#define FRAME_FD 3
#define CONTROL_FD 4
#define FRAME_WIDTH 720U
#define FRAME_HEIGHT 1584U
#define FRAME_SLOTS 2U
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 4U)
#define SHARED_BYTES (FRAME_BYTES * FRAME_SLOTS)

enum message_kind {
  TOUCH_DOWN = 1, TOUCH_MOVE, TOUCH_UP, KEY, CLOSE, ACK,
  FRAME = 32, STATUS, ERROR
};

struct message {
  uint32_t magic;
  uint32_t kind;
  uint32_t sequence;
  int32_t x;
  int32_t y;
  uint32_t value;
  char text[128];
};

enum control {
  CONTROL_NONE = 0,
  CONTROL_UP,
  CONTROL_DOWN,
  CONTROL_LEFT,
  CONTROL_RIGHT,
  CONTROL_FIRE,
  CONTROL_USE,
  CONTROL_MAP,
  CONTROL_MENU,
  CONTROL_WEAPON,
};

static uint8_t *shared_pixels;
static uint32_t sequence;
static bool outstanding;
static volatile bool running = true;
static enum control held_control = CONTROL_NONE;
static pthread_mutex_t engine_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t audio_thread_id;
static bool audio_started;

static uint64_t monotonic_us(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000ULL +
         (uint64_t)now.tv_nsec / 1000ULL;
}

static void sleep_until(uint64_t deadline) {
  for (;;) {
    const uint64_t now = monotonic_us();
    if (now >= deadline) return;
    const uint64_t remaining = deadline - now;
    struct timespec delay = {
      .tv_sec = (time_t)(remaining / 1000000ULL),
      .tv_nsec = (long)((remaining % 1000000ULL) * 1000ULL),
    };
    if (!nanosleep(&delay, &delay) || errno != EINTR) return;
  }
}

static bool send_message(uint32_t kind, const char *text) {
  struct message message;
  memset(&message, 0, sizeof(message));
  message.magic = AERA_MAGIC;
  message.kind = kind;
  if (text) snprintf(message.text, sizeof(message.text), "%s", text);
  const ssize_t count = send(CONTROL_FD, &message, sizeof(message),
                             MSG_DONTWAIT | MSG_NOSIGNAL);
  return count == (ssize_t)sizeof(message);
}

static bool platform_init(void) {
  struct stat info;
  int socket_type = 0;
  socklen_t socket_type_size = sizeof(socket_type);
  if (fstat(FRAME_FD, &info) || !S_ISREG(info.st_mode) ||
      (uint64_t)info.st_size != SHARED_BYTES ||
      getsockopt(CONTROL_FD, SOL_SOCKET, SO_TYPE, &socket_type,
                 &socket_type_size) || socket_type != SOCK_SEQPACKET)
    return false;
  shared_pixels = mmap(NULL, SHARED_BYTES, PROT_READ | PROT_WRITE,
                       MAP_SHARED, FRAME_FD, 0);
  if (shared_pixels == MAP_FAILED) {
    shared_pixels = NULL;
    return false;
  }
  if (fcntl(CONTROL_FD, F_SETFL,
            fcntl(CONTROL_FD, F_GETFL) | O_NONBLOCK))
    return false;
  return true;
}

static enum control control_at(int x, int y) {
  const struct { int x, y, radius; enum control control; } targets[] = {
    {575, 885, 112, CONTROL_FIRE}, {550, 1120, 92, CONTROL_USE},
    {190, 920, 68, CONTROL_UP}, {190, 1220, 68, CONTROL_DOWN},
    {55, 1070, 68, CONTROL_LEFT}, {325, 1070, 68, CONTROL_RIGHT},
    {92, 1365, 66, CONTROL_MENU}, {360, 1365, 82, CONTROL_WEAPON},
    {625, 1365, 66, CONTROL_MAP},
  };
  for (size_t index = 0; index < sizeof(targets) / sizeof(targets[0]); ++index) {
    const int dx = x - targets[index].x;
    const int dy = y - targets[index].y;
    if (dx * dx + dy * dy <= targets[index].radius * targets[index].radius)
      return targets[index].control;
  }
  return CONTROL_NONE;
}

static void set_control(enum control control, bool down) {
  doom_key_t key = DOOM_KEY_UNKNOWN;
  switch (control) {
    case CONTROL_UP: key = DOOM_KEY_UP_ARROW; break;
    case CONTROL_DOWN: key = DOOM_KEY_DOWN_ARROW; break;
    case CONTROL_LEFT: key = DOOM_KEY_LEFT_ARROW; break;
    case CONTROL_RIGHT: key = DOOM_KEY_RIGHT_ARROW; break;
    case CONTROL_FIRE: key = DOOM_KEY_CTRL; break;
    case CONTROL_USE: key = DOOM_KEY_SPACE; break;
    case CONTROL_MAP: key = DOOM_KEY_TAB; break;
    case CONTROL_MENU: key = DOOM_KEY_ESCAPE; break;
    default: break;
  }
  pthread_mutex_lock(&engine_lock);
  if (control == CONTROL_FIRE) {
    if (down) {
      doom_key_down(DOOM_KEY_CTRL);
      doom_key_down(DOOM_KEY_ENTER);
    } else {
      doom_key_up(DOOM_KEY_CTRL);
      doom_key_up(DOOM_KEY_ENTER);
    }
  } else if (control == CONTROL_WEAPON && down) {
    static doom_key_t weapon = DOOM_KEY_1;
    doom_key_down(weapon);
    doom_key_up(weapon);
    weapon = weapon == DOOM_KEY_7 ? DOOM_KEY_1 : (doom_key_t)(weapon + 1);
  } else if (key != DOOM_KEY_UNKNOWN) {
    if (down) doom_key_down(key);
    else doom_key_up(key);
  }
  pthread_mutex_unlock(&engine_lock);
}

static void update_touch(enum control next) {
  if (next == held_control) return;
  if (held_control != CONTROL_NONE) set_control(held_control, false);
  held_control = next;
  if (held_control != CONTROL_NONE) set_control(held_control, true);
  if (held_control == CONTROL_WEAPON) held_control = CONTROL_NONE;
}

static void poll_host(void) {
  for (;;) {
    struct message message;
    const ssize_t count = recv(CONTROL_FD, &message, sizeof(message),
                               MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (count != (ssize_t)sizeof(message) || message.magic != AERA_MAGIC) {
      running = false;
      return;
    }
    if (message.kind == CLOSE) {
      running = false;
      update_touch(CONTROL_NONE);
      return;
    }
    if (message.kind == ACK && outstanding && message.sequence == sequence) {
      outstanding = false;
      continue;
    }
    if (message.x < 0 || message.y < 0 ||
        message.x >= (int32_t)FRAME_WIDTH ||
        message.y >= (int32_t)FRAME_HEIGHT)
      continue;
    if (message.kind == TOUCH_UP) update_touch(CONTROL_NONE);
    else if (message.kind == TOUCH_DOWN || message.kind == TOUCH_MOVE)
      update_touch(control_at(message.x, message.y));
  }
}

static uint32_t blend(uint32_t destination, uint32_t source, unsigned alpha) {
  const unsigned inverse = 255U - alpha;
  const unsigned r = ((((source >> 16) & 255U) * alpha) +
                      (((destination >> 16) & 255U) * inverse)) / 255U;
  const unsigned g = ((((source >> 8) & 255U) * alpha) +
                      (((destination >> 8) & 255U) * inverse)) / 255U;
  const unsigned b = (((source & 255U) * alpha) +
                      ((destination & 255U) * inverse)) / 255U;
  return 0xff000000U | (r << 16) | (g << 8) | b;
}

static void fill_rect(uint32_t *pixels, int left, int top, int right,
                      int bottom, uint32_t color, unsigned alpha) {
  if (left < 0) left = 0;
  if (top < 0) top = 0;
  if (right > (int)FRAME_WIDTH) right = FRAME_WIDTH;
  if (bottom > (int)FRAME_HEIGHT) bottom = FRAME_HEIGHT;
  for (int y = top; y < bottom; ++y)
    for (int x = left; x < right; ++x) {
      uint32_t *pixel = pixels + y * FRAME_WIDTH + x;
      *pixel = alpha == 255 ? color : blend(*pixel, color, alpha);
    }
}

static void fill_circle(uint32_t *pixels, int cx, int cy, int radius,
                        uint32_t color, unsigned alpha) {
  const int radius2 = radius * radius;
  for (int y = -radius; y <= radius; ++y)
    for (int x = -radius; x <= radius; ++x)
      if (x * x + y * y <= radius2) {
        const int px = cx + x, py = cy + y;
        if (px < 0 || py < 0 || px >= (int)FRAME_WIDTH ||
            py >= (int)FRAME_HEIGHT) continue;
        uint32_t *pixel = pixels + py * FRAME_WIDTH + px;
        *pixel = alpha == 255 ? color : blend(*pixel, color, alpha);
      }
}

static const uint8_t font_rows[][7] = {
  {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
  {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
  {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
  {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
  {31,4,4,4,4,4,31},      {7,2,2,2,18,18,12},
  {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
  {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
  {15,16,16,14,1,1,30},   {31,4,4,4,4,4,4},
  {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
  {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
  {17,17,10,4,4,4,4},     {31,1,2,4,8,16,31},
  {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
  {14,17,1,2,4,8,31},     {30,1,1,14,1,1,30},
  {2,6,10,18,31,2,2},     {31,16,16,30,1,1,30},
  {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
  {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
};

static int glyph_index(char character) {
  if (character >= 'A' && character <= 'Z') return character - 'A';
  if (character >= '0' && character <= '9') return 26 + character - '0';
  return -1;
}

static void draw_text(uint32_t *pixels, int x, int y, const char *text,
                      int scale, uint32_t color) {
  for (; *text; ++text, x += 6 * scale) {
    const int glyph = glyph_index(*text);
    if (glyph < 0) continue;
    for (int row = 0; row < 7; ++row)
      for (int column = 0; column < 5; ++column)
        if (font_rows[glyph][row] & (1U << (4 - column)))
          fill_rect(pixels, x + column * scale, y + row * scale,
                    x + (column + 1) * scale, y + (row + 1) * scale,
                    color, 255);
  }
}

static void draw_arrow(uint32_t *pixels, int cx, int cy, enum control control,
                       uint32_t color) {
  for (int n = 0; n < 38; ++n) {
    const int half = n / 2 + 2;
    if (control == CONTROL_UP)
      fill_rect(pixels, cx - half, cy + n - 19, cx + half,
                cy + n - 17, color, 255);
    else if (control == CONTROL_DOWN)
      fill_rect(pixels, cx - half, cy - n + 17, cx + half,
                cy - n + 19, color, 255);
    else if (control == CONTROL_LEFT)
      fill_rect(pixels, cx + n - 19, cy - half, cx + n - 17,
                cy + half, color, 255);
    else if (control == CONTROL_RIGHT)
      fill_rect(pixels, cx - n + 17, cy - half, cx - n + 19,
                cy + half, color, 255);
  }
}

static void draw_button(uint32_t *pixels, int cx, int cy, int radius,
                        const char *label, bool active) {
  const uint32_t accent = 0xff22d3eeU;
  fill_circle(pixels, cx, cy, radius, active ? accent : 0xff202832U,
              active ? 220 : 245);
  fill_circle(pixels, cx, cy, radius - 8, 0xff111820U, active ? 45 : 210);
  const int scale = strlen(label) > 5 ? 3 : 4;
  const int width = (int)strlen(label) * 6 * scale - scale;
  draw_text(pixels, cx - width / 2, cy - 7 * scale / 2, label, scale,
            active ? 0xff061014U : 0xffe8f2f4U);
}

static void compose_frame(uint32_t *output, const unsigned char *source) {
  const uint32_t canvas = 0xff0b1016U;
  const uint32_t accent = 0xff22d3eeU;
  for (size_t index = 0; index < FRAME_WIDTH * FRAME_HEIGHT; ++index)
    output[index] = canvas;

  const int game_top = 96;
  const int game_height = 540;
  for (int y = 0; y < game_height; ++y) {
    const int sy = y * 200 / game_height;
    for (int x = 0; x < (int)FRAME_WIDTH; ++x) {
      const int sx = x * 320 / FRAME_WIDTH;
      const unsigned char *rgba = source + (sy * 320 + sx) * 4;
      output[(game_top + y) * FRAME_WIDTH + x] =
          0xff000000U | ((uint32_t)rgba[0] << 16) |
          ((uint32_t)rgba[1] << 8) | rgba[2];
    }
  }
  fill_rect(output, 0, 650, FRAME_WIDTH, 654, 0xff18232dU, 255);
  draw_text(output, 28, 682, "MOVE", 2, 0xff71808aU);
  draw_text(output, 548, 682, "ACTION", 2, 0xff71808aU);

  fill_circle(output, 190, 1070, 180, 0xff18212aU, 230);
  fill_circle(output, 190, 1070, 70, canvas, 255);
  const struct { int x, y; enum control control; } arrows[] = {
    {190, 920, CONTROL_UP}, {190, 1220, CONTROL_DOWN},
    {55, 1070, CONTROL_LEFT}, {325, 1070, CONTROL_RIGHT},
  };
  for (size_t index = 0; index < sizeof(arrows) / sizeof(arrows[0]); ++index) {
    const bool active = held_control == arrows[index].control;
    fill_circle(output, arrows[index].x, arrows[index].y, 60,
                active ? accent : 0xff293640U, active ? 235 : 220);
    draw_arrow(output, arrows[index].x, arrows[index].y,
               arrows[index].control,
               active ? 0xff061014U : 0xffe8f2f4U);
  }

  draw_button(output, 575, 885, 104, "FIRE", held_control == CONTROL_FIRE);
  draw_button(output, 550, 1120, 84, "USE", held_control == CONTROL_USE);
  draw_button(output, 625, 1365, 58, "MAP", held_control == CONTROL_MAP);
  draw_button(output, 92, 1365, 58, "MENU", held_control == CONTROL_MENU);
  draw_button(output, 360, 1365, 74, "WEAPON",
              held_control == CONTROL_WEAPON);
}

static bool publish_frame(const unsigned char *source) {
  if (outstanding || !shared_pixels) return true;
  const uint32_t next_sequence = sequence + 1U;
  uint32_t *output = (uint32_t *)(shared_pixels +
      (next_sequence % FRAME_SLOTS) * FRAME_BYTES);
  compose_frame(output, source);
  struct message message;
  memset(&message, 0, sizeof(message));
  message.magic = AERA_MAGIC;
  message.kind = FRAME;
  message.sequence = next_sequence;
  message.x = FRAME_WIDTH;
  message.y = FRAME_HEIGHT;
  message.value = FRAME_BYTES;
  const ssize_t count = send(CONTROL_FD, &message, sizeof(message),
                             MSG_DONTWAIT | MSG_NOSIGNAL);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
  if (count != (ssize_t)sizeof(message)) return false;
  sequence = next_sequence;
  outstanding = true;
  return true;
}

static int connect_audio(void) {
  static const char socket_name[] = "aera-browser-audio-v1";
  static const uint32_t hello[] = {0x41525041U, 48000U, 2U, 16U};
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path + 1, socket_name, sizeof(socket_name) - 1);
  const socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
      1 + sizeof(socket_name) - 1);
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || connect(fd, (struct sockaddr *)&address, size) ||
      send(fd, hello, sizeof(hello), MSG_NOSIGNAL) != (ssize_t)sizeof(hello)) {
    if (fd >= 0) close(fd);
    return -1;
  }
  return fd;
}

static bool write_audio(int fd, const void *data, size_t bytes) {
  const uint8_t *cursor = data;
  size_t offset = 0;
  while (offset < bytes && running) {
    const ssize_t count = send(fd, cursor + offset, bytes - offset,
                               MSG_NOSIGNAL);
    if (count > 0) offset += (size_t)count;
    else if (count < 0 && errno == EINTR) continue;
    else return false;
  }
  return offset == bytes;
}

static void *audio_thread(void *unused) {
  (void)unused;
  const int fd = connect_audio();
  if (fd < 0) return NULL;
  int16_t input[1024];
  enum { OUTPUT_FRAMES = 2230 };
  int16_t output[OUTPUT_FRAMES * 2];
  uint64_t next = monotonic_us();
  while (running) {
    pthread_mutex_lock(&engine_lock);
    memcpy(input, doom_get_sound_buffer(), sizeof(input));
    pthread_mutex_unlock(&engine_lock);
    for (int frame = 0; frame < OUTPUT_FRAMES; ++frame) {
      const unsigned source = (unsigned)((uint64_t)frame * 512U /
                                         OUTPUT_FRAMES);
      output[frame * 2] = input[source * 2];
      output[frame * 2 + 1] = input[source * 2 + 1];
    }
    if (!write_audio(fd, output, sizeof(output))) break;
    next += 46440;
    sleep_until(next);
  }
  close(fd);
  return NULL;
}

static const char *select_wad_directory(void) {
  static const char *personal[] = {
    "/doom/doom2.wad", "/doom/doom.wad", "/doom/doom1.wad",
  };
  for (size_t index = 0; index < sizeof(personal) / sizeof(personal[0]); ++index)
    if (!access(personal[index], R_OK)) return "/doom";
  if (!access("/usr/share/aera-doom/doom.wad", R_OK))
    return "/usr/share/aera-doom";
  return NULL;
}

int main(void) {
  if (!platform_init()) return 78;
  const char *wad_directory = select_wad_directory();
  if (!wad_directory) {
    send_message(ERROR, "No Doom or FreeDoom WAD is available.");
    return 66;
  }
  if (setenv("DOOMWADDIR", wad_directory, 1)) return 78;
  if (access("/doom", W_OK) || chdir("/doom")) chdir("/tmp");

  doom_set_resolution(320, 200);
  doom_set_default_int("use_mouse", 0);
  doom_set_default_int("use_joystick", 0);
  doom_set_default_int("always_run", 1);
  doom_set_default_int("crosshair", 1);
  doom_set_default_int("show_messages", 1);
  char *arguments[] = {(char *)"aera-doom", NULL};
  send_message(STATUS, "Starting the standalone Doom engine");
  doom_init(1, arguments, DOOM_FLAG_HIDE_MOUSE_OPTIONS |
                          DOOM_FLAG_HIDE_MUSIC_OPTIONS |
                          DOOM_FLAG_MENU_DARKEN_BG);
  if (pthread_create(&audio_thread_id, NULL, audio_thread, NULL) == 0)
    audio_started = true;

  uint64_t next_frame = monotonic_us();
  while (running) {
    poll_host();
    pthread_mutex_lock(&engine_lock);
    doom_update();
    const unsigned char *framebuffer = doom_get_framebuffer(4);
    const bool published = framebuffer && publish_frame(framebuffer);
    pthread_mutex_unlock(&engine_lock);
    if (!published) break;
    next_frame += 28571;
    const uint64_t now = monotonic_us();
    if (next_frame + 100000 < now) next_frame = now;
    sleep_until(next_frame);
  }
  running = false;
  update_touch(CONTROL_NONE);
  if (audio_started) pthread_join(audio_thread_id, NULL);
  if (shared_pixels) munmap(shared_pixels, SHARED_BYTES);
  return 0;
}
