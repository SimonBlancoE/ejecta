# Muelle

Unidades extraíbles en la bandeja de Noctalia (niri). Sustituye a udiskie.

- **Clic izquierdo**: panel con las unidades (montadas y sin montar), su
  ocupación, detalles (dispositivo, conexión, ruta, UUID, nº de serie…) y
  botones: *Montar*, *Montar y abrir*, *Abrir*, *Desmontar*, *Expulsar*,
  *Desbloquear* (LUKS). Se cierra con `Esc`, volviendo a pulsar el icono o
  al pasar a otra ventana.
- **Clic derecho**: menú con todo lo anterior por unidad, acciones globales
  (montar / desmontar / expulsar todas) y las preferencias.
- **Clic central**: con una sola unidad, la abre en el gestor de archivos.
- **Notificaciones**: al conectar un dispositivo pregunta si montarlo; al
  montarse puede preguntar si abrirlo; al expulsar avisa de que ya se puede
  retirar.

## Desmontar o expulsar

- **Desmontar**: cierra el sistema de archivos. El dispositivo sigue conectado
  y encendido; puedes volver a montarlo.
- **Expulsar**: desmonta todas sus particiones, bloquea las cifradas y apaga
  el dispositivo (o expulsa el disco óptico). Después ya se puede quitar.

## Cuando algo está en uso

Si no se puede desmontar o expulsar porque un programa tiene algo abierto en la
unidad, la notificación dice qué procesos son (nombre, PID y motivo: archivo
abierto, carpeta actual de una terminal, programa ejecutándose desde ella…) y
ofrece *Cerrar procesos y reintentar*. En el panel, los detalles de cada unidad
montada listan esos procesos con un botón *Cerrar* por proceso y *Cerrar
todos*; en el menú del clic derecho está *Cerrar los procesos que la usan*.

Cerrar significa enviar SIGTERM (el programa puede guardar y salir) y, si sigue
vivo a los 3 s, SIGKILL. Solo se ven y se cierran procesos tuyos: si lo bloquea
uno del sistema o de otro usuario, el mensaje lo indica.

## Preferencias

Desde *clic derecho → Preferencias* o en `~/.config/muelle/muelle.ini`:

| Clave              | Por defecto | Qué hace                                             |
|--------------------|-------------|------------------------------------------------------|
| `preguntar_montar` | `true`      | Notificación al conectar: *Montar* / *Montar y abrir* |
| `preguntar_abrir`  | `false`     | Notificación al montarse: *Abrir*                     |
| `mostrar_internas` | `false`     | Lista también particiones de discos internos          |
| `icono_siempre`    | `false`     | Muestra el icono aunque no haya unidades              |

## Compilar e instalar

Solo usa GTK4, GIO y gtk4-layer-shell, ya presentes en el sistema:

```sh
make install          # ~/.local/bin/muelle
```

La sesión niri-noctalia lo arranca desde
`~/Proyectos/niri-noctalia/session/start-removable-media-agent` como
`niri-noctalia-muelle.service`. Si `muelle` no está instalado, se usa udiskie.

Ejecutar `muelle` otra vez abre o cierra el panel (útil para un atajo de niri).
Los colores salen de `~/.config/gtk-4.0/colors.css`, que genera Noctalia, y se
recargan al cambiar el tema.
