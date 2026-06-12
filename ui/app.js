class SteamCloudPatcher {
  constructor() {
    this.baseUrl = window.location.origin;
    this.games = [];
    this.providers = [];
    this.backups = [];
    this.activeFilter = 'all';
    this.searchQuery = '';
    this.searchTimeout = null;
    this.refreshInterval = null;
    this.toastQueue = [];

    this.loadPreferences();
    this.bindEvents();
    this.initParticles();
    this.init();
  }

  loadPreferences() {
    try {
      const prefs = JSON.parse(localStorage.getItem('scp_prefs') || '{}');
      this.prefs = {
        refreshInterval: prefs.refreshInterval ?? 60,
        notifications: prefs.notifications ?? true,
        backupsCollapsed: prefs.backupsCollapsed ?? false,
        ...prefs,
      };
    } catch {
      this.prefs = { refreshInterval: 60, notifications: true, backupsCollapsed: false };
    }
  }

  savePreferences() {
    localStorage.setItem('scp_prefs', JSON.stringify(this.prefs));
  }

  async init() {
    this.applyPreferences();

    await Promise.allSettled([
      this.fetchStatus(),
      this.fetchSteamInfo(),
      this.fetchProviders(),
      this.fetchGames(),
      this.fetchBackups(),
      this.fetchAutoPatchSettings(),
    ]);

    this.setupAutoRefresh();
  }

  applyPreferences() {
    const intervalSelect = document.getElementById('settings-refresh-interval');
    if (intervalSelect) intervalSelect.value = this.prefs.refreshInterval;

    const notifToggle = document.getElementById('settings-notifications');
    if (notifToggle) notifToggle.checked = this.prefs.notifications;

    const backupsSection = document.getElementById('backups-section');
    if (this.prefs.backupsCollapsed) {
      backupsSection.classList.add('collapsed');
    }
  }

  setupAutoRefresh() {
    if (this.refreshInterval) clearInterval(this.refreshInterval);
    const seconds = parseInt(this.prefs.refreshInterval);
    if (seconds > 0) {
      this.refreshInterval = setInterval(() => this.refreshAll(), seconds * 1000);
    }
  }

  async refreshAll() {
    await Promise.allSettled([
      this.fetchStatus(),
      this.fetchGames(),
      this.fetchProviders(),
      this.fetchBackups(),
    ]);
  }

  async apiGet(endpoint) {
    try {
      const res = await fetch(`${this.baseUrl}${endpoint}`);
      if (!res.ok) throw new Error(`HTTP ${res.status}: ${res.statusText}`);
      return await res.json();
    } catch (err) {
      console.error(`GET ${endpoint} failed:`, err);
      throw err;
    }
  }

  async apiPost(endpoint, body = {}) {
    try {
      const res = await fetch(`${this.baseUrl}${endpoint}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (!res.ok) {
        const errData = await res.json().catch(() => ({}));
        throw new Error(errData.error || errData.message || `HTTP ${res.status}`);
      }
      return await res.json();
    } catch (err) {
      console.error(`POST ${endpoint} failed:`, err);
      throw err;
    }
  }

  async fetchStatus() {
    try {
      const data = await this.apiGet('/api/status');
      this.setConnectionStatus(true, data);
    } catch {
      this.setConnectionStatus(false);
    }
  }

  setConnectionStatus(connected, data = null) {
    const dot = document.getElementById('status-dot');
    const text = document.getElementById('status-text');
    dot.className = `status-dot ${connected ? 'connected' : 'disconnected'}`;
    text.textContent = connected ? 'Connected' : 'Disconnected';
  }

  async fetchSteamInfo() {
    try {
      const data = await this.apiGet('/api/steam');
      const detectedPath = data.path || data.steamPath || '';
      document.getElementById('steam-path').textContent = detectedPath || 'Not found';
      document.getElementById('steam-path').title = detectedPath;
      document.getElementById('steam-user').textContent = data.userId || data.steamId || '—';
      document.getElementById('settings-steam-path').value = detectedPath;

      if (detectedPath && !localStorage.getItem('scp_steam_path_confirmed')) {
        setTimeout(async () => {
          const confirmed = await this.showConfirmModal(
            'Confirm Steam Path',
            `We auto-detected your Steam installation path at:<br><br><code>${this.escapeHtml(detectedPath)}</code><br><br>Is this correct?`,
            'info'
          );
          if (confirmed) {
            localStorage.setItem('scp_steam_path_confirmed', 'true');
            this.showToast('Steam installation path confirmed.', 'success');
          } else {
            document.getElementById('settings-panel').classList.remove('hidden');
            document.getElementById('settings-steam-path').focus();
            this.showToast('Please correct your Steam installation path in settings.', 'warning');
          }
        }, 500);
      }
    } catch {
      document.getElementById('steam-path').textContent = 'Not detected';
    }
  }

  async fetchProviders() {
    try {
      const data = await this.apiGet('/api/providers');
      this.providers = Array.isArray(data) ? data : (data.providers || []);
      this.renderProviders();
    } catch {
      this.renderProvidersError();
    }
  }

  renderProviders() {
    const providerMap = {
      gdrive: { dotId: 'gdrive-status-dot', textId: 'gdrive-status-text', pathId: 'gdrive-path' },
      google_drive: { dotId: 'gdrive-status-dot', textId: 'gdrive-status-text', pathId: 'gdrive-path' },
      googledrive: { dotId: 'gdrive-status-dot', textId: 'gdrive-status-text', pathId: 'gdrive-path' },
      onedrive: { dotId: 'onedrive-status-dot', textId: 'onedrive-status-text', pathId: 'onedrive-path' },
      local: { dotId: 'local-status-dot', textId: 'local-status-text', pathId: 'local-path' },
    };

    Object.values(providerMap).forEach(({ dotId, textId, pathId }) => {
      const dot = document.getElementById(dotId);
      const text = document.getElementById(textId);
      const path = document.getElementById(pathId);
      if (dot) { dot.className = 'status-dot-sm not-detected'; }
      if (text) text.textContent = 'Not found';
      if (path) { path.textContent = '—'; path.title = ''; }
    });

    this.providers.forEach(p => {
      const key = (p.id || p.name || '').toLowerCase().replace(/[\s_-]/g, '');
      let mapping = null;

      for (const [k, v] of Object.entries(providerMap)) {
        if (key.includes(k.replace(/_/g, ''))) {
          mapping = v;
          break;
        }
      }

      if (mapping) {
        const dot = document.getElementById(mapping.dotId);
        const text = document.getElementById(mapping.textId);
        const path = document.getElementById(mapping.pathId);

        const detected = p.detected || p.available || p.installed || false;

        if (dot) dot.className = `status-dot-sm ${detected ? 'detected' : 'not-detected'}`;
        if (text) text.textContent = detected ? 'Detected' : 'Not found';
        if (path && p.path) {
          path.textContent = p.path;
          path.title = p.path;
        }
      }
    });
  }

  renderProvidersError() {
    ['gdrive', 'onedrive', 'local'].forEach(id => {
      const dot = document.getElementById(`${id}-status-dot`);
      const text = document.getElementById(`${id}-status-text`);
      if (dot) dot.className = 'status-dot-sm not-detected';
      if (text) text.textContent = 'Error';
    });
  }

  async fetchGames() {
    try {
      const data = await this.apiGet('/api/games');
      this.games = Array.isArray(data) ? data : (data.games || []);
      document.getElementById('games-count').textContent = this.games.length;
      this.renderGames();
    } catch {
      this.games = [];
      this.renderGames();
    }
  }

  renderGames() {
    const grid = document.getElementById('games-grid');
    const emptyState = document.getElementById('empty-state');
    const filtered = this.getFilteredGames();

    document.getElementById('games-section-count').textContent = filtered.length;

    if (filtered.length === 0) {
      grid.innerHTML = '';
      emptyState.classList.remove('hidden');
      return;
    }

    emptyState.classList.add('hidden');

    grid.innerHTML = filtered.map((game, i) => {
      const appId = game.appId || game.AppId || game.app_id || '';
      const name = game.name || game.Name || game.gameName || 'Unknown Game';
      const developer = game.developer || game.Developer || game.publisher || '';
      const savePath = game.resolvedSavePath || game.savePath || game.SavePath || game.save_path || '—';
      const patched = game.patched || game.Patched || game.isPatched || false;
      const error = game.error || game.Error || false;

      let statusClass = 'not-patched';
      let statusLabel = 'Not Patched';
      let statusIcon = '○';
      if (error) {
        statusClass = 'error';
        statusLabel = 'Error';
        statusIcon = '⚠';
      } else if (patched) {
        statusClass = 'patched';
        statusLabel = 'Patched';
        statusIcon = '✓';
      }

      const selectedProvider = game.provider || game.Provider || '';
      const showCustomPath = selectedProvider === 'local';

      return `
        <div class="game-card" data-appid="${appId}" style="animation-delay: ${i * 0.04}s">
          <div class="game-card-header">
            <div>
              <div class="game-name">${this.escapeHtml(name)}</div>
              ${developer ? `<div class="game-developer">${this.escapeHtml(developer)}</div>` : ''}
            </div>
            <span class="game-appid">${appId}</span>
          </div>
          <div class="game-save-path" title="${this.escapeHtml(savePath)}">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>
            <span>${this.escapeHtml(savePath)}</span>
          </div>
          <div class="game-status">
            <span class="status-badge ${statusClass}">${statusIcon} ${statusLabel}</span>
          </div>
          <div class="game-controls">
            <div class="provider-select-group">
              <select class="provider-select" data-appid="${appId}" onchange="app.onProviderChange(this)">
                <option value="">Select provider...</option>
                <option value="gdrive" ${selectedProvider === 'gdrive' ? 'selected' : ''}>Google Drive</option>
                <option value="onedrive" ${selectedProvider === 'onedrive' ? 'selected' : ''}>OneDrive</option>
                <option value="local" ${selectedProvider === 'local' ? 'selected' : ''}>Local Folder</option>
              </select>
            </div>
            <input type="text" class="custom-path-input ${showCustomPath ? 'visible' : ''}" data-appid="${appId}" placeholder="Custom save path..." value="${this.escapeHtml(game.customPath || '')}">
          </div>
          <div class="game-actions">
            ${patched ? `
              <button class="btn btn-warning btn-flex btn-sm" onclick="app.restoreGame('${appId}', '${this.escapeHtml(name)}')">
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>
                Restore
              </button>
            ` : `
              <button class="btn btn-primary btn-flex btn-sm" onclick="app.patchGame('${appId}', '${this.escapeHtml(name)}')">
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>
                Patch
              </button>
            `}
          </div>
        </div>
      `;
    }).join('');
  }

  getFilteredGames() {
    let result = [...this.games];

    switch (this.activeFilter) {
      case 'installed':
        result = result.filter(g => g.installed !== false);
        break;
      case 'patched':
        result = result.filter(g => g.patched || g.Patched || g.isPatched);
        break;
    }

    if (this.searchQuery) {
      const q = this.searchQuery.toLowerCase();
      result = result.filter(g => {
        const name = (g.name || g.Name || g.gameName || '').toLowerCase();
        const dev = (g.developer || g.Developer || '').toLowerCase();
        const appId = String(g.appId || g.AppId || g.app_id || '');
        return name.includes(q) || dev.includes(q) || appId.includes(q);
      });
    }

    return result;
  }

  onProviderChange(selectEl) {
    const appId = selectEl.dataset.appid;
    const card = selectEl.closest('.game-card');
    const customInput = card.querySelector('.custom-path-input');

    if (selectEl.value === 'local') {
      customInput.classList.add('visible');
    } else {
      customInput.classList.remove('visible');
    }
  }

  async patchGame(appId, gameName) {
    const card = document.querySelector(`.game-card[data-appid="${appId}"]`);
    const providerSelect = card?.querySelector('.provider-select');
    const customInput = card?.querySelector('.custom-path-input');
    const provider = providerSelect?.value;

    if (!provider) {
      this.showToast('Please select a cloud provider first.', 'warning');
      return;
    }

    const confirmed = await this.showConfirmModal(
      'Patch Game',
      `Redirect saves for <strong>${gameName}</strong> to <strong>${this.getProviderLabel(provider)}</strong>?`,
      'warning'
    );

    if (!confirmed) return;

    const btn = card.querySelector('.btn-primary');
    if (btn) {
      btn.disabled = true;
      btn.innerHTML = '<span class="spinner"></span> Patching...';
    }

    try {
      const body = { provider };
      if (provider === 'local' && customInput?.value) {
        body.customPath = customInput.value;
      }

      await this.apiPost(`/api/games/${appId}/patch`, body);
      this.showToast(`Successfully patched ${gameName}!`, 'success');
      await this.fetchGames();
      await this.fetchBackups();
    } catch (err) {
      this.showToast(`Failed to patch ${gameName}: ${err.message}`, 'error');
      if (btn) {
        btn.disabled = false;
        btn.innerHTML = `
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>
          Patch
        `;
      }
    }
  }

  async restoreGame(appId, gameName) {
    const confirmed = await this.showConfirmModal(
      'Restore Game',
      `Restore original save location for <strong>${gameName}</strong>? This will undo the cloud patch.`,
      'warning'
    );

    if (!confirmed) return;

    const card = document.querySelector(`.game-card[data-appid="${appId}"]`);
    const btn = card?.querySelector('.btn-warning');
    if (btn) {
      btn.disabled = true;
      btn.innerHTML = '<span class="spinner"></span> Restoring...';
    }

    try {
      await this.apiPost(`/api/games/${appId}/restore`);
      this.showToast(`Successfully restored ${gameName}!`, 'success');
      await this.fetchGames();
      await this.fetchBackups();
    } catch (err) {
      this.showToast(`Failed to restore ${gameName}: ${err.message}`, 'error');
      if (btn) {
        btn.disabled = false;
        btn.innerHTML = `
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>
          Restore
        `;
      }
    }
  }

  async patchAll() {
    const unpatched = this.games.filter(g => !(g.patched || g.Patched || g.isPatched));
    if (unpatched.length === 0) {
      this.showToast('All games are already patched!', 'info');
      return;
    }

    const confirmed = await this.showConfirmModal(
      'Patch All Games',
      `This will patch <strong>${unpatched.length}</strong> game(s). Each game needs a provider selected. Continue?`,
      'warning'
    );

    if (!confirmed) return;

    let successCount = 0;
    let failCount = 0;

    for (const game of unpatched) {
      const appId = game.appId || game.AppId || game.app_id;
      const card = document.querySelector(`.game-card[data-appid="${appId}"]`);
      const provider = card?.querySelector('.provider-select')?.value;

      if (!provider) {
        failCount++;
        continue;
      }

      try {
        const body = { provider };
        const customInput = card?.querySelector('.custom-path-input');
        if (provider === 'local' && customInput?.value) {
          body.customPath = customInput.value;
        }
        await this.apiPost(`/api/games/${appId}/patch`, body);
        successCount++;
      } catch {
        failCount++;
      }
    }

    if (successCount > 0) {
      this.showToast(`Patched ${successCount} game(s) successfully!`, 'success');
    }
    if (failCount > 0) {
      this.showToast(`${failCount} game(s) failed or had no provider selected.`, 'warning');
    }

    await this.fetchGames();
    await this.fetchBackups();
  }

  async restartSteam() {
    const confirmed = await this.showConfirmModal(
      'Restart Steam',
      'Are you sure you want to restart Steam? This will close Steam and launch it again to apply save patches and sync correctly.',
      'warning'
    );

    if (!confirmed) return;

    const btn = document.getElementById('btn-restart-steam');
    let originalHtml = '';
    if (btn) {
      originalHtml = btn.innerHTML;
      btn.disabled = true;
      btn.innerHTML = '<span class="spinner"></span> Restarting...';
    }

    try {
      await this.apiPost('/api/steam/restart');
      this.showToast('Steam restart command sent. Steam is restarting...', 'success');
    } catch (err) {
      this.showToast(`Failed to restart Steam: ${err.message}`, 'error');
    } finally {
      if (btn) {
        btn.disabled = false;
        btn.innerHTML = originalHtml;
      }
    }
  }

  async fixAllSyncErrors() {
    const confirmed = await this.showConfirmModal(
      'Fix Cloud Sync Errors',
      'This will clear Steam Cloud metadata cache and refresh timestamps for ALL patched games. It is highly recommended to restart Steam after this. Continue?',
      'warning'
    );

    if (!confirmed) return;

    const btn = document.getElementById('btn-fix-all-sync');
    let originalHtml = '';
    if (btn) {
      originalHtml = btn.innerHTML;
      btn.disabled = true;
      btn.innerHTML = '<span class="spinner"></span> Fixing...';
    }

    try {
      const res = await this.apiPost('/api/games/fix-all');
      this.showToast(res.message || 'Successfully touched save files and reset Steam Cloud cache.', 'success');
      await this.fetchGames();
    } catch (err) {
      this.showToast(`Failed to fix sync errors: ${err.message}`, 'error');
    } finally {
      if (btn) {
        btn.disabled = false;
        btn.innerHTML = originalHtml;
      }
    }
  }

  async fetchBackups() {
    try {
      const data = await this.apiGet('/api/backups');
      this.backups = Array.isArray(data) ? data : (data.backups || []);
      this.renderBackups();
    } catch {
      this.backups = [];
      this.renderBackups();
    }
  }

  renderBackups() {
    const tbody = document.getElementById('backups-tbody');
    const countBadge = document.getElementById('backups-count');

    countBadge.textContent = this.backups.length;

    if (this.backups.length === 0) {
      tbody.innerHTML = '<tr class="empty-row"><td colspan="4">No backups found.</td></tr>';
      return;
    }

    tbody.innerHTML = this.backups.map(b => {
      const gameName = b.gameName || b.game || b.name || 'Unknown';
      const date = b.date || b.createdAt || b.timestamp || '—';
      const path = b.path || b.backupPath || '—';
      const appId = b.appId || b.AppId || '';

      const formattedDate = this.formatDate(date);

      return `
        <tr>
          <td><strong>${this.escapeHtml(gameName)}</strong></td>
          <td>${formattedDate}</td>
          <td><span class="backup-path" title="${this.escapeHtml(path)}">${this.escapeHtml(path)}</span></td>
          <td>
            <button class="btn btn-ghost btn-sm" onclick="app.restoreBackup('${appId}', '${this.escapeHtml(gameName)}')">
              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>
              Restore
            </button>
          </td>
        </tr>
      `;
    }).join('');
  }

  async restoreBackup(appId, gameName) {
    if (!appId) return;
    await this.restoreGame(appId, gameName);
  }

  async saveSteamPath() {
    const input = document.getElementById('settings-steam-path');
    const path = input.value.trim();

    if (!path) {
      this.showToast('Please enter a valid Steam path.', 'warning');
      return;
    }

    try {
      await this.apiPost('/api/settings/steam-path', { path });
      this.showToast('Steam path updated successfully!', 'success');
      await this.fetchSteamInfo();
      await this.fetchGames();
    } catch (err) {
      this.showToast(`Failed to update Steam path: ${err.message}`, 'error');
    }
  }

  async fetchAutoPatchSettings() {
    try {
      const data = await this.apiGet('/api/settings/autopatch');
      document.getElementById('settings-autopatch').checked = data.autoPatch;
      document.getElementById('settings-default-provider').value = data.provider || '';
      document.getElementById('settings-custom-path').value = data.customPath || '';
      this.toggleSettingsCustomPathVisibility(data.provider);
      document.getElementById('settings-enable-backup').checked = data.enableBackup;
      document.getElementById('settings-backup-path').value = data.backupPath || '';
      this.toggleSettingsBackupPathVisibility(data.enableBackup);
    } catch (err) {
      console.error(err);
    }
  }

  async saveAutoPatchSettings() {
    const autoPatch = document.getElementById('settings-autopatch').checked;
    const provider = document.getElementById('settings-default-provider').value;
    const customPath = document.getElementById('settings-custom-path').value.trim();
    const enableBackup = document.getElementById('settings-enable-backup').checked;
    const backupPath = document.getElementById('settings-backup-path').value.trim();

    try {
      await this.apiPost('/api/settings/autopatch', { autoPatch, provider, customPath, enableBackup, backupPath });
      this.showToast('Auto-Patch settings updated.', 'success');
      await this.fetchGames();
    } catch (err) {
      this.showToast(`Failed to save settings: ${err.message}`, 'error');
    }
  }

  toggleSettingsCustomPathVisibility(provider) {
    const group = document.getElementById('settings-custom-path-group');
    if (provider === 'local') {
      group.style.display = 'block';
    } else {
      group.style.display = 'none';
    }
  }

  toggleSettingsBackupPathVisibility(enableBackup) {
    const group = document.getElementById('settings-backup-path-group');
    if (enableBackup) {
      group.style.display = 'block';
    } else {
      group.style.display = 'none';
    }
  }

  showToast(message, type = 'info') {
    if (!this.prefs.notifications && type !== 'error') return;

    const container = document.getElementById('toast-container');
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;

    const iconSvg = this.getToastIcon(type);

    toast.innerHTML = `
      <span class="toast-icon">${iconSvg}</span>
      <span class="toast-message">${message}</span>
      <button class="toast-close" onclick="this.closest('.toast').remove()">×</button>
    `;

    container.appendChild(toast);

    setTimeout(() => {
      toast.classList.add('removing');
      setTimeout(() => toast.remove(), 300);
    }, 4000);
  }

  getToastIcon(type) {
    switch (type) {
      case 'success':
        return '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><polyline points="22 4 12 14.01 9 11.01"/></svg>';
      case 'error':
        return '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>';
      case 'warning':
        return '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>';
      default:
        return '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>';
    }
  }

  showConfirmModal(title, message, type = 'warning') {
    return new Promise(resolve => {
      const overlay = document.getElementById('modal-overlay');
      const modalIcon = document.getElementById('modal-icon');
      const modalTitle = document.getElementById('modal-title');
      const modalMessage = document.getElementById('modal-message');
      const confirmBtn = document.getElementById('modal-confirm');
      const cancelBtn = document.getElementById('modal-cancel');

      modalTitle.textContent = title;
      modalMessage.innerHTML = message;
      modalIcon.className = `modal-icon ${type}`;

      if (type === 'warning') {
        modalIcon.innerHTML = '<svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>';
        confirmBtn.className = 'btn btn-warning';
      } else if (type === 'info') {
        modalIcon.innerHTML = '<svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>';
        confirmBtn.className = 'btn btn-primary';
      } else {
        modalIcon.innerHTML = '<svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>';
        confirmBtn.className = 'btn btn-danger';
      }

      overlay.classList.remove('hidden');

      const cleanup = () => {
        overlay.classList.add('hidden');
        confirmBtn.removeEventListener('click', onConfirm);
        cancelBtn.removeEventListener('click', onCancel);
        overlay.removeEventListener('click', onOverlayClick);
        document.removeEventListener('keydown', onKey);
      };

      const onConfirm = () => { cleanup(); resolve(true); };
      const onCancel = () => { cleanup(); resolve(false); };
      const onOverlayClick = (e) => { if (e.target === overlay) { cleanup(); resolve(false); } };
      const onKey = (e) => {
        if (e.key === 'Escape') { cleanup(); resolve(false); }
        if (e.key === 'Enter') { cleanup(); resolve(true); }
      };

      confirmBtn.addEventListener('click', onConfirm);
      cancelBtn.addEventListener('click', onCancel);
      overlay.addEventListener('click', onOverlayClick);
      document.addEventListener('keydown', onKey);
    });
  }

  bindEvents() {
    document.getElementById('btn-settings').addEventListener('click', () => {
      document.getElementById('settings-panel').classList.toggle('hidden');
    });

    document.getElementById('btn-close-settings').addEventListener('click', () => {
      document.getElementById('settings-panel').classList.add('hidden');
    });

    document.getElementById('btn-refresh').addEventListener('click', () => {
      const btn = document.getElementById('btn-refresh');
      btn.style.transform = 'rotate(360deg)';
      btn.style.transition = 'transform 0.6s ease';
      setTimeout(() => { btn.style.transform = ''; btn.style.transition = ''; }, 600);
      this.refreshAll();
      this.showToast('Refreshing data...', 'info');
    });

    document.getElementById('btn-edit-path').addEventListener('click', () => {
      const panel = document.getElementById('settings-panel');
      panel.classList.remove('hidden');
      setTimeout(() => document.getElementById('settings-steam-path').focus(), 300);
    });

    document.getElementById('btn-save-steam-path').addEventListener('click', () => {
      this.saveSteamPath();
    });

    document.getElementById('settings-refresh-interval').addEventListener('change', (e) => {
      this.prefs.refreshInterval = parseInt(e.target.value);
      this.savePreferences();
      this.setupAutoRefresh();
      this.showToast('Auto-refresh interval updated.', 'info');
    });

    document.getElementById('settings-notifications').addEventListener('change', (e) => {
      this.prefs.notifications = e.target.checked;
      this.savePreferences();
    });

    document.getElementById('settings-autopatch').addEventListener('change', () => {
      this.saveAutoPatchSettings();
    });

    document.getElementById('settings-default-provider').addEventListener('change', (e) => {
      this.toggleSettingsCustomPathVisibility(e.target.value);
      this.saveAutoPatchSettings();
    });

    document.getElementById('settings-enable-backup').addEventListener('change', (e) => {
      this.toggleSettingsBackupPathVisibility(e.target.checked);
      this.saveAutoPatchSettings();
    });

    document.getElementById('settings-backup-path').addEventListener('change', () => {
      this.saveAutoPatchSettings();
    });

    document.getElementById('settings-custom-path').addEventListener('change', () => {
      this.saveAutoPatchSettings();
    });

    document.getElementById('filter-tabs').addEventListener('click', (e) => {
      const tab = e.target.closest('.filter-tab');
      if (!tab) return;

      document.querySelectorAll('.filter-tab').forEach(t => t.classList.remove('active'));
      tab.classList.add('active');
      this.activeFilter = tab.dataset.filter;
      this.renderGames();
    });

    document.getElementById('search-input').addEventListener('input', (e) => {
      clearTimeout(this.searchTimeout);
      this.searchTimeout = setTimeout(() => {
        this.searchQuery = e.target.value.trim();
        this.renderGames();
      }, 250);
    });

    document.getElementById('backups-header').addEventListener('click', () => {
      const section = document.getElementById('backups-section');
      section.classList.toggle('collapsed');
      this.prefs.backupsCollapsed = section.classList.contains('collapsed');
      this.savePreferences();
    });

    document.getElementById('btn-patch-all').addEventListener('click', () => {
      this.patchAll();
    });

    document.getElementById('btn-restart-steam').addEventListener('click', () => {
      this.restartSteam();
    });

    document.getElementById('btn-fix-all-sync').addEventListener('click', () => {
      this.fixAllSyncErrors();
    });

    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        document.getElementById('settings-panel').classList.add('hidden');
      }
    });
  }

  initParticles() {
    const canvas = document.getElementById('particles-canvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    let particles = [];
    let animFrame;

    const resize = () => {
      canvas.width = window.innerWidth;
      canvas.height = window.innerHeight;
    };

    resize();
    window.addEventListener('resize', resize);

    const count = Math.min(40, Math.floor(window.innerWidth / 30));
    for (let i = 0; i < count; i++) {
      particles.push({
        x: Math.random() * canvas.width,
        y: Math.random() * canvas.height,
        r: Math.random() * 1.2 + 0.3,
        dx: (Math.random() - 0.5) * 0.3,
        dy: (Math.random() - 0.5) * 0.3,
        opacity: Math.random() * 0.3 + 0.05,
      });
    }

    const draw = () => {
      ctx.clearRect(0, 0, canvas.width, canvas.height);

      particles.forEach(p => {
        ctx.beginPath();
        ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2);
        ctx.fillStyle = `rgba(0, 212, 255, ${p.opacity})`;
        ctx.fill();

        p.x += p.dx;
        p.y += p.dy;

        if (p.x < -10) p.x = canvas.width + 10;
        if (p.x > canvas.width + 10) p.x = -10;
        if (p.y < -10) p.y = canvas.height + 10;
        if (p.y > canvas.height + 10) p.y = -10;
      });

      for (let i = 0; i < particles.length; i++) {
        for (let j = i + 1; j < particles.length; j++) {
          const dx = particles[i].x - particles[j].x;
          const dy = particles[i].y - particles[j].y;
          const dist = Math.sqrt(dx * dx + dy * dy);

          if (dist < 150) {
            ctx.beginPath();
            ctx.moveTo(particles[i].x, particles[i].y);
            ctx.lineTo(particles[j].x, particles[j].y);
            ctx.strokeStyle = `rgba(0, 212, 255, ${0.03 * (1 - dist / 150)})`;
            ctx.lineWidth = 0.5;
            ctx.stroke();
          }
        }
      }

      animFrame = requestAnimationFrame(draw);
    };

    draw();

    window.addEventListener('beforeunload', () => {
      cancelAnimationFrame(animFrame);
    });
  }

  escapeHtml(str) {
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
  }

  getProviderLabel(provider) {
    const labels = {
      gdrive: 'Google Drive',
      onedrive: 'OneDrive',
      local: 'Local Folder',
    };
    return labels[provider] || provider;
  }

  formatDate(dateStr) {
    if (!dateStr || dateStr === '—') return '—';
    try {
      const d = new Date(dateStr);
      if (isNaN(d.getTime())) return dateStr;
      return d.toLocaleDateString('en-US', {
        year: 'numeric',
        month: 'short',
        day: 'numeric',
        hour: '2-digit',
        minute: '2-digit',
      });
    } catch {
      return dateStr;
    }
  }
}

const app = new SteamCloudPatcher();
