#include <cstdio>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

int main() {
#if defined(__unix__) || defined(__APPLE__)
  struct winsize ws;
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
      ws.ws_col > 0) {
    std::printf("%u %u\n", ws.ws_row, ws.ws_col);
    return 0;
  }
  if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
      ws.ws_col > 0) {
    std::printf("%u %u\n", ws.ws_row, ws.ws_col);
    return 0;
  }
#endif
  return 1;
}
