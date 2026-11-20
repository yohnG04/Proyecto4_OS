// minidb.h: datos y funciones comunes de MiniDB (Proyecto 4).
// Sistemas Operativos 2026-2, Universidad EAFIT.
//
// Este archivo lo entrega el docente y NO se modifica. Define la tabla de
// cuentas, el generador determinista de datos y de transferencias, la
// firma de auditoría y el checksum. Como todos los grupos usan el mismo
// generador, los resultados de las consultas y los checksums son los
// mismos en todas las entregas y el script de pruebas puede verificarlos.
//
// Uso: #include "user/minidb.h" en user/minidb.c (después de user/user.h).

#define NCUENTAS     8192   // registros de la tabla (8192 x 32 B = 64 páginas)
#define NSUCURSALES  8      // sucursales de la cooperativa
#define MAXWORKERS   8      // máximo de procesos trabajadores en una consulta
#define FIRMA_RONDAS 10000   // costo de la firma de auditoría por cuenta
#define NACTIVAS     1024   // cuentas más activas (las primeras 8 páginas)
#define SNAP_ARCHIVO "snap.db"
#define SNAP_MAGIA   0x534e4150 // "SNAP"

// Un registro de la tabla. Ocupa exactamente 32 bytes, así que una página
// de 4096 bytes guarda 128 cuentas.
struct cuenta {
  int id;            // 0 .. NCUENTAS-1, igual a su posición en la tabla
  int sucursal;      // 0 .. NSUCURSALES-1
  int saldo;         // en miles de pesos, siempre >= 0
  int nmov;          // movimientos aplicados a la cuenta
  char titular[16];  // nombre corto, terminado en '\0'
};

// Resultado parcial de una consulta para una sucursal.
struct agregado {
  int cuentas;       // número de cuentas
  int saldo_max;     // saldo máximo
  uint64 saldo_total;// suma de saldos
  uint firmas;       // suma (módulo 2^32) de las firmas de auditoría
};

// Lo que cada trabajador envía al padre por su pipe.
struct resultado {
  int pid;           // pid del trabajador (obtenido con ugetpid())
  int desde, hasta;  // rango de cuentas procesado: [desde, hasta)
  struct agregado suc[NSUCURSALES];
};

// Encabezado del archivo de snapshot. Después del encabezado vienen
// las n cuentas, tal como están en memoria.
struct snap_encabezado {
  uint magia;        // SNAP_MAGIA
  uint n;            // número de cuentas guardadas (NCUENTAS)
  uint checksum;     // checksum_tabla() de las cuentas guardadas
  uint reservado;    // 0
};

// Generador pseudoaleatorio (LCG). Avanza la semilla y devuelve 31 bits.
static inline uint
mdb_aleatorio(uint *semilla)
{
  *semilla = *semilla * 1103515245u + 12345u;
  return (*semilla >> 1) & 0x7fffffff;
}

// Llena la cuenta i de la tabla. Siempre produce el mismo resultado.
static inline void
generar_cuenta(int i, struct cuenta *c)
{
  static const char *nombres[8] = {
    "ana", "luis", "marta", "jorge", "sofia", "pedro", "lina", "camilo"
  };
  uint s = 2026u * 7919u + (uint)i * 2654435761u;
  c->id = i;
  c->sucursal = mdb_aleatorio(&s) % NSUCURSALES;
  c->saldo = 100 + mdb_aleatorio(&s) % 50000;
  c->nmov = 0;
  const char *n = nombres[mdb_aleatorio(&s) % 8];
  int k = 0;
  for(; n[k] && k < 10; k++)
    c->titular[k] = n[k];
  c->titular[k++] = '-';
  int num = i % 10000;
  c->titular[k++] = '0' + (num / 1000) % 10;
  c->titular[k++] = '0' + (num / 100) % 10;
  c->titular[k++] = '0' + (num / 10) % 10;
  c->titular[k++] = '0' + num % 10;
  while(k < 16)
    c->titular[k++] = 0;
}

// Firma de auditoría de una cuenta. Es deliberadamente costosa en CPU
// (FIRMA_RONDAS iteraciones) para que la consulta paralela tenga trabajo
// que repartir entre los trabajadores.
static inline uint
firma(const struct cuenta *c)
{
  const unsigned char *b = (const unsigned char *)c;
  uint h = 2166136261u;
  for(int r = 0; r < FIRMA_RONDAS; r++)
    for(int k = 0; k < 16; k++){
      h ^= b[k] + r;
      h *= 16777619u;
    }
  return h;
}

// Checksum de n cuentas (Fletcher de 32 bits sobre palabras de 32 bits).
static inline uint
checksum_tabla(const struct cuenta *t, int n)
{
  const uint *w = (const uint *)t;
  uint a = 1, b = 0;
  int nw = n * (int)(sizeof(struct cuenta) / sizeof(uint));
  for(int i = 0; i < nw; i++){
    a = (a + w[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

// Genera la siguiente transferencia de la secuencia. Empiecen con
// semilla = 4 y llamen a la función M veces seguidas. Como en una
// cooperativa real, casi todo el movimiento (255 de cada 256
// transferencias) ocurre entre las NACTIVAS cuentas más activas; el resto
// involucra cualquier cuenta de la tabla.
struct transferencia {
  int origen, destino, monto;
};

static inline void
generar_transferencia(uint *semilla, struct transferencia *t)
{
  int rango = (mdb_aleatorio(semilla) % 256) ? NACTIVAS : NCUENTAS;
  t->origen = mdb_aleatorio(semilla) % rango;
  t->destino = mdb_aleatorio(semilla) % rango;
  t->monto = 1 + mdb_aleatorio(semilla) % 500;
}

// Reserva espacio para la tabla con sbrklazy() (Proyecto 3), alineado a
// página para que pgaccess() pueda consultar sus páginas.
// Devuelve 0 si no hay memoria.
static inline struct cuenta *
reservar_tabla(void)
{
  char *fin = sbrklazy(0);
  uint64 relleno = (PGSIZE - ((uint64)fin % PGSIZE)) % PGSIZE;
  if(relleno && sbrklazy(relleno) == (char *)-1)
    return 0;
  char *t = sbrklazy(NCUENTAS * sizeof(struct cuenta));
  if(t == (char *)-1)
    return 0;
  return (struct cuenta *)t;
}
