// cowtest: pruebas del Proyecto 4 (Copy-on-Write fork y MiniDB).
// Sistemas Operativos 2026-2, Universidad EAFIT.
//
// Uso dentro de xv6:
//   $ cowtest            ejecuta todas las pruebas
//   $ cowtest <nombre>   ejecuta solo la prueba indicada
//   $ cowtest -l         lista las pruebas disponibles
//
// Cada prueba corre en un proceso hijo. Si el kernel mata a ese proceso
// o la prueba encuentra un error, se reporta FALLO y se sigue con la
// siguiente. Al final se imprime un resumen.
//
// Algunas pruebas provocan escrituras inválidas o agotan la memoria a
// propósito. Por eso es normal ver mensajes del kernel como
//   usertrap(): unexpected scause 0xf pid=...
// siempre que la prueba correspondiente termine en OK.

#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "kernel/cowstat.h"
#include "user/user.h"

#define SBRK_FALLO ((char *)-1)

static char *nombre_actual = "";

static void
fallo(char *motivo)
{
  printf("prueba %s: FALLO: %s\n", nombre_actual, motivo);
  exit(1);
}

static struct cowstat
leer_stats(void)
{
  struct cowstat st;
  if(cowstat(&st) < 0)
    fallo("cowstat() devolvió -1");
  return st;
}

static uint64
paginas_libres(void)
{
  return leer_stats().freepages;
}

// Espera a que terminen n hijos y devuelve cuántos terminaron con estado 0.
static int
esperar_hijos(int n)
{
  int ok = 0;
  for(int i = 0; i < n; i++){
    int estado;
    if(wait(&estado) < 0)
      fallo("wait() falló");
    if(estado == 0)
      ok++;
  }
  return ok;
}

// Pide npaginas con sbrk() normal y escribe un valor en cada una.
static char *
reservar(int npaginas, int valor)
{
  char *a = sbrk(npaginas * PGSIZE);
  if(a == SBRK_FALLO)
    fallo("sbrk() falló");
  for(int i = 0; i < npaginas; i++)
    *(int *)(a + i * PGSIZE) = valor + i;
  return a;
}

static void
liberar(int npaginas)
{
  if(sbrk(-npaginas * PGSIZE) == SBRK_FALLO)
    fallo("sbrk() negativo falló");
}

// 1. Un proceso que usa más de la mitad de la memoria libre puede hacer
// fork(). Sin copy-on-write el fork no tiene memoria para copiarlo.
static void
fork_grande(void)
{
  int n = (paginas_libres() * 2) / 3;
  char *a = reservar(n, 1000);
  int pid = fork();
  if(pid < 0)
    fallo("fork() falló con un proceso grande (¿se están copiando las páginas?)");
  if(pid == 0){
    for(int i = 0; i < n; i += 97)
      if(*(int *)(a + i * PGSIZE) != 1000 + i)
        exit(1);
    exit(0);
  }
  if(esperar_hijos(1) != 1)
    fallo("el hijo no vio los datos del padre");
  liberar(n);
}

// 2. Después de fork(), cada proceso ve solo sus propias escrituras.
static void
aislamiento(void)
{
  int n = 16;
  char *a = reservar(n, 0);
  int pid = fork();
  if(pid < 0)
    fallo("fork() falló");
  if(pid == 0){
    for(int i = 0; i < n; i++)
      if(*(int *)(a + i * PGSIZE) != i)
        exit(1);
    for(int i = 0; i < n; i++)
      *(int *)(a + i * PGSIZE) = 5000 + i;
    for(int i = 0; i < n; i++)
      if(*(int *)(a + i * PGSIZE) != 5000 + i)
        exit(2);
    exit(0);
  }
  if(esperar_hijos(1) != 1)
    fallo("el hijo no vio sus propios datos");
  for(int i = 0; i < n; i++)
    if(*(int *)(a + i * PGSIZE) != i)
      fallo("la escritura del hijo cambió la memoria del padre");
  // El padre también puede escribir después del fork.
  for(int i = 0; i < n; i++)
    *(int *)(a + i * PGSIZE) = 7000 + i;
  for(int i = 0; i < n; i++)
    if(*(int *)(a + i * PGSIZE) != 7000 + i)
      fallo("el padre no puede escribir después del fork");
  liberar(n);
}

// 3. Abuelo, padre e hijo comparten y escriben las mismas páginas a la vez.
static void
tres_niveles(void)
{
  int n = 32;
  uint64 libres0 = paginas_libres();
  char *a = reservar(n, 0);
  int pid = fork();
  if(pid < 0)
    fallo("fork() falló");
  if(pid == 0){
    int pid2 = fork();
    if(pid2 < 0)
      exit(1);
    int marca = pid2 == 0 ? 3000 : 2000;
    for(int r = 0; r < 20; r++)
      for(int i = 0; i < n; i++)
        *(int *)(a + i * PGSIZE + 8) = marca + i;
    for(int i = 0; i < n; i++)
      if(*(int *)(a + i * PGSIZE + 8) != marca + i || *(int *)(a + i * PGSIZE) != i)
        exit(1);
    if(pid2 == 0)
      exit(0);
    exit(esperar_hijos(1) == 1 ? 0 : 1);
  }
  for(int r = 0; r < 20; r++)
    for(int i = 0; i < n; i++)
      *(int *)(a + i * PGSIZE + 8) = 1000 + i;
  if(esperar_hijos(1) != 1)
    fallo("un descendiente vio datos de otro proceso");
  for(int i = 0; i < n; i++)
    if(*(int *)(a + i * PGSIZE + 8) != 1000 + i)
      fallo("un descendiente cambió la memoria del abuelo");
  liberar(n);
  if(paginas_libres() != libres0)
    fallo("la memoria libre no volvió al valor inicial");
}

// 4. El kernel escribe con copyout() en una página CoW (read() de un pipe).
static char buf_global[3 * PGSIZE];

static void
copyout_cow(void)
{
  memset(buf_global, 'p', sizeof(buf_global));
  int fds[2];
  if(pipe(fds) < 0)
    fallo("pipe() falló");
  int pid = fork();
  if(pid < 0)
    fallo("fork() falló");
  if(pid == 0){
    close(fds[1]);
    // El buffer cruza tres páginas compartidas con el padre.
    char *destino = buf_global + PGSIZE - 100;
    int total = 0;
    while(total < PGSIZE + 200){
      int r = read(fds[0], destino + total, PGSIZE + 200 - total);
      if(r <= 0)
        exit(1);
      total += r;
    }
    for(int i = 0; i < PGSIZE + 200; i++)
      if(destino[i] != 'h')
        exit(2);
    exit(0);
  }
  close(fds[0]);
  char datos[512];
  memset(datos, 'h', sizeof(datos));
  int falta = PGSIZE + 200;
  while(falta > 0){
    int k = falta < sizeof(datos) ? falta : sizeof(datos);
    if(write(fds[1], datos, k) != k)
      fallo("write() falló");
    falta -= k;
  }
  close(fds[1]);
  if(esperar_hijos(1) != 1)
    fallo("read() no pudo escribir en una página compartida del hijo");
  for(int i = 0; i < sizeof(buf_global); i++)
    if(buf_global[i] != 'p')
      fallo("read() del hijo escribió en la memoria del padre");

  // Ahora el padre lee de un pipe sobre sus páginas, que siguen marcadas CoW.
  if(pipe(fds) < 0)
    fallo("pipe() falló");
  if(write(fds[1], "xv6", 3) != 3 || read(fds[0], buf_global + 10, 3) != 3)
    fallo("read() del padre sobre una página CoW falló");
  close(fds[0]);
  close(fds[1]);
  if(memcmp(buf_global + 10, "xv6", 3) != 0)
    fallo("read() del padre no dejó los datos");
}

// 5. Copy-on-write convive con la asignación perezosa (sbrklazy).
static void
perezosa_cow(void)
{
  int n = 8;
  char *a = sbrklazy(n * PGSIZE);
  if(a == SBRK_FALLO)
    fallo("sbrklazy() falló");
  for(int i = 0; i < n; i += 2)
    a[i * PGSIZE] = 'a' + i;           // páginas 0, 2, 4, 6 asignadas
  int pid = fork();
  if(pid < 0)
    fallo("fork() falló");
  if(pid == 0){
    if(a[1 * PGSIZE] != 0)             // perezosa: debe leerse en ceros
      exit(1);
    if(a[2 * PGSIZE] != 'c')
      exit(2);
    a[2 * PGSIZE] = 'Z';               // CoW
    a[3 * PGSIZE] = 'Y';               // perezosa
    if(a[2 * PGSIZE] != 'Z' || a[3 * PGSIZE] != 'Y')
      exit(3);
    exit(0);
  }
  if(esperar_hijos(1) != 1)
    fallo("el hijo no vio bien las páginas perezosas o compartidas");
  if(a[2 * PGSIZE] != 'c')
    fallo("la escritura del hijo en una página CoW cambió al padre");
  if(a[3 * PGSIZE] != 0)
    fallo("la página perezosa del hijo apareció en el padre");
  if(sbrk(-n * PGSIZE) == SBRK_FALLO)
    fallo("sbrk() negativo falló");
}

// 6. La página USYSCALL del Proyecto 3 es propia de cada proceso:
// no se comparte con fork().
static void
ugetpid_hijo(void)
{
  int padre = getpid();
  for(int i = 0; i < 4; i++){
    int pid = fork();
    if(pid < 0)
      fallo("fork() falló");
    if(pid == 0){
      if(ugetpid() != getpid() || ugetpid() == padre)
        exit(1);
      int pid2 = fork();
      if(pid2 == 0)
        exit(ugetpid() == getpid() ? 0 : 1);
      exit(esperar_hijos(1) == 1 ? 0 : 1);
    }
  }
  if(esperar_hijos(4) != 4)
    fallo("ugetpid() no coincide con getpid() en un hijo (¿se compartió USYSCALL?)");
  if(ugetpid() != padre)
    fallo("ugetpid() del padre cambió");
}

// 7. Las páginas que eran de solo lectura siguen siéndolo después de fork().
static void
escribir_texto(void)
{
  *(volatile int *)(uint64)escribir_texto = 0;
}

static void
escribir_usyscall(void)
{
  *(volatile int *)USYSCALL = 0;
}

static void
solo_lectura(void)
{
  void (*f[2])(void) = {escribir_texto, escribir_usyscall};
  for(int i = 0; i < 2; i++){
    int pid = fork();
    if(pid < 0)
      fallo("fork() falló");
    if(pid == 0){
      f[i]();
      exit(0);
    }
    int estado;
    wait(&estado);
    if(estado != -1)
      fallo(i == 0 ? "el hijo pudo escribir en su código (texto)"
                   : "el hijo pudo escribir en USYSCALL");
  }
}

// 8. cowstat() cuenta los fallos y las copias de cada proceso.
static void
contadores(void)
{
  int n = 10;
  char *a = reservar(n, 0);
  int fds[2], listo[2];
  if(pipe(fds) < 0 || pipe(listo) < 0)
    fallo("pipe() falló");
  int pid = fork();
  if(pid < 0)
    fallo("fork() falló");
  if(pid == 0){
    char c;
    close(fds[1]);
    close(listo[0]);
    // Las primeras escrituras del hijo (por ejemplo en su pila) también
    // son fallos CoW; se descuentan midiendo dos veces antes de empezar.
    struct cowstat s0 = leer_stats();
    if(s0.cowfaults > 4)
      exit(1);                         // un hijo nuevo empieza en cero
    s0 = leer_stats();
    s0 = leer_stats();
    for(int i = 0; i < n; i++)
      a[i * PGSIZE] = 'x';
    struct cowstat s1 = leer_stats();
    if(s1.cowcopies - s0.cowcopies != n || s1.cowfaults - s0.cowfaults != n)
      exit(2);                         // una copia por página escrita
    a[0] = 'y';
    if(leer_stats().cowfaults != s1.cowfaults)
      exit(3);                         // la página ya es propia: no hay fallo
    write(listo[1], "k", 1);
    read(fds[0], &c, 1);               // esperar a que el padre mida
    exit(0);
  }
  char c;
  close(fds[0]);
  close(listo[1]);
  // Si el hijo falla antes de avisar, read() devuelve 0 y no se mide.
  if(read(listo[0], &c, 1) == 1){
    struct cowstat s = leer_stats();
    if(s.sharedpages < 4)
      fallo("sharedpages no refleja las páginas compartidas con el hijo");
  }
  write(fds[1], "f", 1);
  int estado;
  wait(&estado);
  if(estado != 0)
    fallo(estado == 1 ? "los contadores de un hijo nuevo no empiezan en 0" :
          estado == 2 ? "cowcopies o cowfaults no cuentan una copia por página escrita" :
          estado == 3 ? "se contó un fallo en una página que ya era propia" :
          "el kernel mató al hijo");
  // El hijo terminó: el padre es el único dueño y no debe copiar.
  struct cowstat p0 = leer_stats();
  for(int i = 0; i < n; i++)
    a[i * PGSIZE] = 'z';
  struct cowstat p1 = leer_stats();
  if(p1.cowfaults - p0.cowfaults != n)
    fallo("el padre debería atender un fallo CoW por página");
  if(p1.cowcopies != p0.cowcopies)
    fallo("se copió una página que tenía una sola referencia");
  close(fds[1]);
  close(listo[0]);
  liberar(n);
}

// 9. Muchos fork()/exit() seguidos no pierden memoria.
static void
sin_fugas(void)
{
  int n = 20;
  char *a = reservar(n, 0);
  uint64 libres0 = paginas_libres();
  for(int r = 0; r < 60; r++){
    int pid = fork();
    if(pid < 0)
      fallo("fork() falló");
    if(pid == 0){
      for(int i = r % 3; i < n; i += 3)
        a[i * PGSIZE] = r;
      exit(0);
    }
    if(r % 2)
      a[(r % n) * PGSIZE] = r;         // el padre también escribe a veces
    if(esperar_hijos(1) != 1)
      fallo("un hijo falló");
  }
  if(paginas_libres() != libres0)
    fallo("la memoria libre no volvió al valor inicial (fuga de páginas)");
  liberar(n);
}

// 10. Si las copias agotan la memoria, el kernel mata al proceso
// en vez de entrar en pánico, y la memoria se recupera.
static void
sin_memoria(void)
{
  int n = paginas_libres() / 2;
  char *a = reservar(n, 0);
  uint64 libres0 = paginas_libres();
  for(int k = 0; k < 3; k++){
    int pid = fork();
    if(pid < 0)
      fallo("fork() falló");
    if(pid == 0){
      for(int i = 0; i < n; i++)
        a[i * PGSIZE + 4] = 1;         // copia todo: no cabe para los tres
      exit(0);
    }
  }
  if(esperar_hijos(3) == 3)
    fallo("los tres hijos copiaron más memoria de la que existe");
  if(paginas_libres() != libres0)
    fallo("la memoria no se recuperó después de quedarse sin memoria");
  liberar(n);
}

// 11. Varios procesos escriben a la vez en las mismas páginas compartidas
// desde varias CPUs; el conteo de referencias debe estar protegido.
static void
concurrente(void)
{
  int n = 48, hijos = 4;
  char *a = reservar(n, 0);
  uint64 libres0 = paginas_libres();
  for(int r = 0; r < 15; r++){
    for(int h = 0; h < hijos; h++){
      int pid = fork();
      if(pid < 0)
        fallo("fork() falló");
      if(pid == 0){
        for(int i = 0; i < n; i++)
          *(int *)(a + i * PGSIZE + 16) = h * 1000 + i;
        for(int i = 0; i < n; i++)
          if(*(int *)(a + i * PGSIZE + 16) != h * 1000 + i || *(int *)(a + i * PGSIZE) != i)
            exit(1);
        exit(0);
      }
    }
    if(esperar_hijos(hijos) != hijos)
      fallo("un hijo vio datos de otro hijo");
  }
  if(paginas_libres() != libres0)
    fallo("la memoria libre no volvió al valor inicial");
  liberar(n);
}

struct prueba {
  void (*f)(void);
  char *nombre;
} pruebas[] = {
  {fork_grande, "fork_grande"},
  {aislamiento, "aislamiento"},
  {tres_niveles, "tres_niveles"},
  {copyout_cow, "copyout_cow"},
  {perezosa_cow, "perezosa_cow"},
  {ugetpid_hijo, "ugetpid_hijo"},
  {solo_lectura, "solo_lectura"},
  {contadores, "contadores"},
  {sin_fugas, "sin_fugas"},
  {sin_memoria, "sin_memoria"},
  {concurrente, "concurrente"},
  {0, 0},
};

static int
correr(struct prueba *p)
{
  nombre_actual = p->nombre;
  int pid = fork();
  if(pid < 0){
    printf("prueba %s: FALLO: fork() falló\n", p->nombre);
    return 0;
  }
  if(pid == 0){
    p->f();
    exit(0);
  }
  int estado = 99;
  if(wait(&estado) != pid){
    // wait() escribe el estado con copyout() en la pila (compartida
    // copy-on-write): si falla, la prueba no se puede dar por buena.
    printf("prueba %s: FALLO: wait() no pudo devolver el estado del hijo\n", p->nombre);
    return 0;
  }
  if(estado == 0){
    printf("prueba %s: OK\n", p->nombre);
    return 1;
  }
  if(estado == -1)
    printf("prueba %s: FALLO: el kernel mató al proceso de la prueba\n", p->nombre);
  return 0;
}

int
main(int argc, char *argv[])
{
  char *solo = 0;
  if(argc > 1){
    if(strcmp(argv[1], "-l") == 0){
      for(struct prueba *p = pruebas; p->f; p++)
        printf("%s\n", p->nombre);
      exit(0);
    }
    solo = argv[1];
  }

  printf("cowtest: inicio\n");
  int total = 0, ok = 0;
  for(struct prueba *p = pruebas; p->f; p++){
    if(solo && strcmp(solo, p->nombre) != 0)
      continue;
    total++;
    ok += correr(p);
  }
  if(total == 0){
    printf("cowtest: no existe la prueba %s (use cowtest -l)\n", solo);
    exit(1);
  }
  if(ok == total){
    printf("cowtest: TODAS LAS PRUEBAS PASARON (%d de %d)\n", ok, total);
    exit(0);
  }
  printf("cowtest: FALLARON %d de %d PRUEBAS\n", total - ok, total);
  exit(1);
}
