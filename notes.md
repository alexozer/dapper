# toggle redshift

pkill -x -USR1 redshift

# todo

Remove systray
Write dapper.cpp
Configure tmux
New vim statusbar?
Move statusbar to bottom?
st instead of gnome-terminal for popups?

# ideas

implement on top of bspwm for sanity

internal workspaces
1: monocle main viewing workspace
2: non-monocle workspace for split viewing
3: dummy workspace for gnome-terminal instance
4+: "special" workspaces for other apps

When an app's keybinding is pressed
launch the app if it doesn't already exist
move it to workspace 1
move all windows off workspace 2
focus it
When shift + app's keybinding pressed
launch app if doesn't exist
move to workspace 1
move to workspace 2
Other apps:
move to one of workspace 4+, depending on whether they are occupied
1 workspace per application class

App bindings:
web browser: i
terminal: n
virtual programs: d and w

# dapper.cpp

arranges apps across desktops as requested
