import "./style.css";

document.querySelector<HTMLDivElement>("#app")!.innerHTML = `
  <div class="h-screen flex flex-col scanline">
    <!-- Header -->
    <header class="bg-cyber-panel/80 backdrop-blur border-b border-cyber-neon/30 px-6 py-3 flex items-center justify-between">
      <div class="flex items-center gap-3">
        <div class="text-2xl font-bold tracking-widest cyber-glow text-cyber-neon">ZYRAX</div>
        <div class="text-xs text-cyber-magenta/70">v0.1.0</div>
      </div>
      <div class="flex gap-4 text-xs">
        <button class="px-3 py-1 border border-cyber-neon/50 hover:bg-cyber-neon/10 hover:shadow-neon-cyan transition-all">_</button>
        <button class="px-3 py-1 border border-cyber-magenta/50 hover:bg-cyber-magenta/10 hover:shadow-neon-magenta transition-all">□</button>
        <button class="px-3 py-1 border border-cyber-pink/50 hover:bg-cyber-pink/10 transition-all">×</button>
      </div>
    </header>

    <!-- Main Content -->
    <main class="flex-1 flex overflow-hidden">
      <!-- Sidebar -->
      <aside class="w-64 bg-cyber-panel/60 border-r border-cyber-cyan/30 flex flex-col">
        <nav class="flex-1 p-4 space-y-2">
          <button class="w-full text-left px-4 py-2 bg-cyber-neon/10 border border-cyber-neon/50 text-cyber-neon shadow-neon-cyan text-sm">
            [ DASHBOARD ]
          </button>
          <button class="w-full text-left px-4 py-2 border border-cyber-cyan/30 hover:bg-cyber-cyan/10 hover:border-cyber-cyan/50 text-cyber-cyan text-sm transition-all">
            [ UPSCALING ]
          </button>
          <button class="w-full text-left px-4 py-2 border border-cyber-magenta/30 hover:bg-cyber-magenta/10 hover:border-cyber-magenta/50 text-cyber-magenta text-sm transition-all">
            [ FRAME GEN ]
          </button>
          <button class="w-full text-left px-4 py-2 border border-cyber-green/30 hover:bg-cyber-green/10 hover:border-cyber-green/50 text-cyber-green text-sm transition-all">
            [ OVERLAY ]
          </button>
          <button class="w-full text-left px-4 py-2 border border-cyber-orange/30 hover:bg-cyber-orange/10 hover:border-cyber-orange/50 text-cyber-orange text-sm transition-all">
            [ PROFILES ]
          </button>
          <button class="w-full text-left px-4 py-2 border border-gray-500/30 hover:bg-gray-500/10 hover:border-gray-500/50 text-gray-400 text-sm transition-all">
            [ SETTINGS ]
          </button>
        </nav>
        <div class="p-4 border-t border-cyber-cyan/30 text-xs text-cyber-cyan/60">
          <div>SYSTEM: ONLINE</div>
          <div class="pulse-glow">HOOK: STANDBY</div>
        </div>
      </aside>

      <!-- Content -->
      <section class="flex-1 p-6 overflow-auto">
        <div class="mb-6">
          <h1 class="text-xl font-bold text-cyber-neon cyber-glow mb-2">[ SYSTEM STATUS ]</h1>
          <div class="h-px bg-gradient-to-r from-cyber-neon via-cyber-magenta to-transparent"></div>
        </div>

        <div class="grid grid-cols-2 gap-4 mb-6">
          <div class="bg-cyber-panel/60 border border-cyber-neon/40 p-4 hover:shadow-neon-cyan transition-all">
            <div class="text-xs text-cyber-neon/70 mb-2">FSR SUPPORT</div>
            <div class="text-2xl font-bold text-cyber-neon">READY</div>
            <div class="text-xs text-gray-400 mt-1">AMD FidelityFX Super Resolution</div>
          </div>
          <div class="bg-cyber-panel/60 border border-cyber-magenta/40 p-4 hover:shadow-neon-magenta transition-all">
            <div class="text-xs text-cyber-magenta/70 mb-2">DLSS SUPPORT</div>
            <div class="text-2xl font-bold text-cyber-magenta">READY</div>
            <div class="text-xs text-gray-400 mt-1">NVIDIA Deep Learning Super Sampling</div>
          </div>
        </div>

        <div class="bg-cyber-panel/60 border border-cyber-cyan/40 p-4 mb-6">
          <div class="text-sm text-cyber-cyan mb-3">[ ACTIVE PROFILE ]</div>
          <div class="flex items-center justify-between">
            <div>
              <div class="text-lg text-white">NONE</div>
              <div class="text-xs text-gray-400">No game detected</div>
            </div            <button class="px-4 py-2 border border-cyber-green/50 text-cyber-green hover:bg-cyber-green/10 hover:shadow-neon-cyan transition-all text-sm">
              SCAN GAMES
            </button>
          </div>
        </div>

        <div class="bg-cyber-panel/60 border border-cyber-pink/40 p-4">
          <div class="text-sm text-cyber-pink mb-3">[ OVERLAY STATUS ]</div>
          <div class="flex items-center justify-between">
            <div class="flex items-center gap-2">
              <div class="w-2 h-2 bg-gray-500 rounded-full"></div>
              <span class="text-gray-400">DISABLED</span>
            </div>
            <button class="px-4 py-2 border border-cyber-pink/50 text-cytaguery\x cx-cyber-pink hover:bg-cyber-pink/10 hover:shadow-neon-pink transition-all text-sm">
              ENABLE OVERLAY
            </button>
          </div>
        </div>
      </section>
    </main>
  </div>
`;
