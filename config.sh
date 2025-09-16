#!/bin/sh
# Configuração do MyWM via IPC (wmctl)

# atalhos para simplificar
w() { wmctl "$@"; }

### Workspaces ###
w set-workspaces "1:dev" "2:web" "3:term" "4:media" "5:misc"

### Keybindings ###
w bind "Mod4-Return"   "spawn st"
w bind "Mod4-d"        "spawn dmenu_run"
w bind "Mod4-q"        "close"
w bind "Mod4-Shift-r"  "reload-config"
w bind "Mod4-Shift-e"  "quit"

# Navegação e foco
w bind "Mod4-h" "focus left"
w bind "Mod4-l" "focus right"
w bind "Mod4-j" "focus down"
w bind "Mod4-k" "focus up"

# Movimentação
w bind "Mod4-Shift-h" "move left"
w bind "Mod4-Shift-l" "move right"
w bind "Mod4-Shift-j" "move down"
w bind "Mod4-Shift-k" "move up"

# Resize teclado
w bind "Mod4-Ctrl-h" "resize -20x 0y"
w bind "Mod4-Ctrl-l" "resize +20x 0y"
w bind "Mod4-Ctrl-j" "resize 0x +20y"
w bind "Mod4-Ctrl-k" "resize 0x -20y"

# Floating toggle
w bind "Mod4-t" "float toggle"

# Workspaces (view/send)
for i in 1 2 3 4 5; do
  w bind "Mod4-$i" "view ws $i"
  w bind "Mod4-Shift-$i" "send ws $i"
done

### Rules ###
# Firefox sempre no workspace 2
w rule "class=Firefox" "workspace=2"
# Gimp sempre em floating
w rule "class=Gimp" "float=true"
# Terminal scratchpad
w scratch "term:st"

### Aparência ###
w set-gap 10
w set-border inner 3
w set-border outer 6
w set-color inner "#444444"
w set-color outer "#88ccff"

### Barra ###
# mostrar só workspaces ocupados
w bar show-occupied-only true
# toggle bar
w bind "Mod4-b" "togglebar"

### Fullscreen ###
w bind "Mod4-f" "fullscreen toggle"

### Scratchpad ###
w bind "Mod4-minus" "scratch toggle term"

### Multi-monitor ###
# Manda Firefox para o monitor 1
w rule "class=Firefox" "monitor=1"
# Move workspace 3 para monitor 1
w bind "Mod4-Shift-3" "move-ws 3 monitor 1"

### Teste de spawn em área ###
w spawn "st" "workspace=1 area=1"
