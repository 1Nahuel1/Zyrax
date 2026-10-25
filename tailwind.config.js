/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        cyber: {
          bg: '#0a0a0f',
          panel: '#11111a',
          neon: '#00fff9',
          magenta: '#ff00ff',
          cyan: '#00e0ff',
          pink: '#ff3366',
          green: '#00ff88',
          orange: '#ff6b00',
        }
      },
      boxShadow: {
        'neon-cyan': '0 0 5px #00fff9, 0 0 20px #00fff9',
        'neon-magenta': '0 0 5px #ff00ff, 0 0 20px #ff00ff',
        'neon-pink': '0 0 5px #ff3366, 0 0 20px #ff3366',
      },
      fontFamily: {
        mono: ['JetBrains Mono', 'Fira Code', 'monospace'],
      }
    },
  },
  plugins: [],
}
