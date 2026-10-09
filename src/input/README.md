# Entrada del DualShock 4

`controller.c` inicia Pad y UserService, abre el control del usuario inicial y lee su estado una vez por cuadro. `pressed` contiene los cambios de suelto a pulsado; `data` conserva los botones sostenidos, palancas y gatillos. Ante errores, neutraliza la entrada y vuelve a intentar abrir el control. Su comportamiento real en PS4 12.00 sigue pendiente.
