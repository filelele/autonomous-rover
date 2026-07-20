#!/bin/bash

# Ensure the script is run with sudo or root privileges
if [ "$EUID" -ne 0 ]; then
  echo "Error: Please run this script with sudo."
  exit 1
 incendium
fi

g++ -Wall -std=c++20 -O3 -I include/ -o main_listener src/main_listener.cpp src/motor_control.cpp -lpigpio

SERVICE_NAME="rover_pi_listener_daemon"
EXEC_NAME="main_listener"

CURRENT_DIR=$(pwd)
EXEC_PATH="$CURRENT_DIR/$EXEC_NAME"

if [ ! -f "$EXEC_PATH" ]; then
  echo "Error: Executable '$EXEC_NAME' not found in $CURRENT_DIR"
  echo "Please compile your program first."
  exit 1
fi

chmod +x "$EXEC_PATH"

cat <<EOF > /etc/systemd/system/${SERVICE_NAME}.service
[Unit]
Description=Rover motor controlling listener
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=${CURRENT_DIR}
ExecStart=${EXEC_PATH}
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

# Register and start
systemctl daemon-reload
systemctl enable ${SERVICE_NAME}.service
systemctl start ${SERVICE_NAME}.service
