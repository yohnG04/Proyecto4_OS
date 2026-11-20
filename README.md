# Proyecto 4 — Copy-on-Write y MiniDB (xv6-riscv)

La solución implementa fork Copy-on-Write, conteo concurrente de referencias, `cowstat()` y snapshot consistente para MiniDB. Se preparó sobre xv6-riscv commit `06aad25` con el parche base del Proyecto 3.

## Integrantes

- Jeronimo Escobar
- Sebastian Cañon Cuartas
- Yohn Alexander Gaviria Hernandez

## Validación ejecutada

Compilación completada con la toolchain RISC-V. xv6 arrancó en QEMU con 3 CPUs.

- `cowtest`: PASÓ, 11/11 pruebas.
- MiniDB: PASÓ en las consultas K=1, 2, 3 y 8; `snapshot 2000` produjo 1978 transferencias aplicadas, 22 rechazadas, checksum inicial `0x6915D295`, checksum final `0x4AB3E209` y 30 copias CoW; `snapshot 0` no hizo copias; `mapa` mostró PTE_COW antes y PTE_W después.
- `pgtbltest`: PASÓ, 11/11 pruebas.
- `usertests -q`: PASÓ, `ALL TESTS PASSED`.

Las salidas auténticas completas están en `pruebas/`. El reporte PDF incluye resultados y ticks medidos con 1 y 3 CPUs.

## Archivos clave

- `kernel/kalloc.c`: referencias y liberación segura de páginas compartidas.
- `kernel/vm.c`, `kernel/trap.c`: fork CoW y resolución de escrituras, incluido `copyout()`.
- `kernel/cowstat.h` y syscall relacionada: contadores de fallos, copias, páginas compartidas y libres.
- `user/minidb.c`: snapshot y validación del archivo.
- `user/cowtest.c`, `user/pgtbltest.c`, `user/usertests.c`: suites incluidas.

## Preguntas para la sustentación

Las cinco respuestas técnicas están en `REPORTE.pdf`.


