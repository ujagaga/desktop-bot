#!/usr/bin/env bash
# Restart the services install.sh set up (recognition first: the gateway runs after it).
set -euo pipefail

SERVICES=(face-recognition.service face-conversation.service)

sudo systemctl restart "${SERVICES[@]}"
sleep 2
for service in "${SERVICES[@]}"; do
    echo "$service: $(systemctl is-active "$service" || true)"
done
echo "Logs: sudo journalctl -u face-conversation.service -n 40"
