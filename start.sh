#!/usr/bin/env bash
# =======================================================
# Escape Room Control System — One-Click Setup & Launch
# =======================================================
# Works on: Linux (Ubuntu/Pop!_OS/Debian), macOS
# Usage: bash start.sh  OR  chmod +x start.sh && ./start.sh
# =======================================================

set -e

echo ""
echo "================================================================"
echo " 🔐 ESCAPE ROOM CONTROL SYSTEM - One-Click Setup & Launch"
echo "================================================================"
echo ""

# ---------------------------------------------------------------
# 1. CHECK NODE.JS IS INSTALLED
# ---------------------------------------------------------------
if ! command -v node &> /dev/null; then
    echo "❌ Node.js is NOT installed!"
    echo ""
    echo "Install it with ONE command:"
    echo ""
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        echo "  curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -"
        echo "  sudo apt-get install -y nodejs"
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        echo "  brew install node"
    fi
    echo ""
    echo "Then re-run this script."
    exit 1
fi

echo "✅ Node.js found: $(node --version)"
echo ""

# ---------------------------------------------------------------
# 2. INSTALL DEPENDENCIES
# ---------------------------------------------------------------
if [ ! -d "node_modules" ]; then
    echo "📦 First-time setup: Installing dependencies..."
    npm install
    echo ""
    echo "✅ Dependencies installed."
    echo ""
else
    echo "✅ Dependencies already installed."
    echo ""
fi

# ---------------------------------------------------------------
# 3. BUILD FRONTEND DASHBOARD
# ---------------------------------------------------------------
echo "🔨 Building React dashboard..."
npm run build
echo ""
echo "✅ Dashboard built."
echo ""

# ---------------------------------------------------------------
# 4. LAUNCH SERVER
# ---------------------------------------------------------------
echo "================================================================"
echo " ⚡ LAUNCHING ESCAPE ROOM CONTROL SERVER"
echo "================================================================"
echo ""
echo " Press Ctrl+C to stop the server."
echo ""

# Auto-open browser after 3 seconds (background)
(sleep 3 && {
    if command -v xdg-open &> /dev/null; then
        xdg-open "http://localhost:3000" 2>/dev/null
    elif command -v open &> /dev/null; then
        open "http://localhost:3000"
    fi
}) &

node index.js
