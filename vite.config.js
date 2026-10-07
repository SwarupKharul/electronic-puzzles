import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  server: {
    host: true, // Listen on all network addresses (0.0.0.0) for mobile/LAN access
    port: 5173,
    proxy: {
      '/api': 'http://localhost:3000',
      '/audio': 'http://localhost:3000',
      '/mqtt': {
        target: 'ws://localhost:3000',
        ws: true
      }
    }
  }
});
