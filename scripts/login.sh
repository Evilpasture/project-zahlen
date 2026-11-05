#!/usr/bin/env bash

set -e
set -o pipefail

# Initialize variables
TUNNEL=""
PASSWORD=""

# Parse command-line arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --tunnel) 
            TUNNEL="$2"
            shift 
            ;;
        --password) 
            PASSWORD="$2"
            shift 
            ;;
        *) 
            echo "Unknown parameter passed: $1"
            exit 1 
            ;;
    esac
    shift
done

# Validate that both arguments were provided
if [[ -z "$TUNNEL" ]] || [[ -z "$PASSWORD" ]]; then
    echo "Error: Missing required arguments."
    echo "Usage: $0 --tunnel <tunnel-name> --password <password>"
    exit 1
fi

# Clean up any lingering cloudflared processes to free up ports/connections
if pgrep -x "cloudflared" > /dev/null; then
    echo "Stopping existing cloudflared process(es)..."
    pkill -x "cloudflared" || true
    sleep 1
fi

echo "Starting Cloudflare tunnel for: ${TUNNEL}..."

# Run cloudflared in the background (&) so the script can continue
cloudflared access tcp --hostname "${TUNNEL}.trycloudflare.com" --url localhost:2223 &
CLOUDFLARED_PID=$!

# Give the tunnel a moment to establish the local listener connection
sleep 2

echo "Connecting via SSH..."
sshpass -v -p "$PASSWORD" ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no -p 2223 frienduser@localhost

# Clean up the background cloudflared process when SSH exits
kill "$CLOUDFLARED_PID" 2>/dev/null || true
