// service-ple: lecturas al azar de 4 KB alineadas (lo que cuesta cada fila que el agente sin servidor lee del disco).
// Uso: service-ple-lecturas fichero hilos segundos [directo 0/1]
// Salida: lecturas/s. Referencia en dcc-gr6 (NVMe): XFS 426.000, ZFS con recordsize 4K 95.000, ZFS con 1 MB unas 3.000.
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) { std::fprintf(stderr, "uso: %s fichero hilos segundos [directo 0/1]\n", argv[0]); return 2; }
  const char* ruta = argv[1];
  const int hilos = std::atoi(argv[2]), segundos = std::atoi(argv[3]);
  const bool directo = argc == 5 && std::atoi(argv[4]) != 0;
  if (hilos < 1 || segundos < 1) { std::fprintf(stderr, "hilos y segundos: al menos 1\n"); return 2; }
  const int fd = ::open(ruta, O_RDONLY | O_CLOEXEC | (directo ? O_DIRECT : 0));
  if (fd < 0) { std::perror(ruta); return 1; }
  struct stat st{};
  ::fstat(fd, &st);
  unsigned long long tam = static_cast<unsigned long long>(st.st_size);
  if (S_ISBLK(st.st_mode)) ::ioctl(fd, BLKGETSIZE64, &tam);
  if (tam < 4096) { std::fprintf(stderr, "%s: menos de 4 KB\n", ruta); return 1; }
  std::atomic<bool> parar{false};
  std::atomic<long> hechas{0}, fallos{0};
  std::vector<std::thread> t;
  for (int i = 0; i < hilos; ++i)
    t.emplace_back([&, i] {
      std::mt19937_64 g(99 + i);
      std::uniform_int_distribution<unsigned long long> d(0, tam / 4096 - 1);
      void* b = nullptr;
      if (::posix_memalign(&b, 4096, 4096) != 0) { ++fallos; return; }
      while (!parar) {
        if (::pread(fd, b, 4096, static_cast<off_t>(d(g) * 4096)) == 4096) ++hechas; else ++fallos;
      }
      std::free(b);
    });
  std::this_thread::sleep_for(std::chrono::seconds(segundos));
  parar = true;
  for (auto& x : t) x.join();
  std::printf("%s  hilos %d  directo %d  lecturas/s %.0f  fallos %ld\n", ruta, hilos, directo ? 1 : 0, hechas / double(segundos), fallos.load());
  ::close(fd);
  return fallos ? 1 : 0;
}
