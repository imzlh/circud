const elements = {
  authView: document.querySelector('#authView'),
  authStatus: document.querySelector('#authStatus'),
  authRetryButton: document.querySelector('#authRetryButton'),
  loginForm: document.querySelector('#loginForm'),
  loginUsername: document.querySelector('#loginUsername'),
  loginPassword: document.querySelector('#loginPassword'),
  loginButton: document.querySelector('#loginButton'),
  loginError: document.querySelector('#loginError'),
  managementApp: document.querySelector('#managementApp'),
  account: document.querySelector('#account'),
  accountName: document.querySelector('#accountName'),
  logoutButton: document.querySelector('#logoutButton'),
  connection: document.querySelector('#connection'),
  connectionText: document.querySelector('#connectionText'),
  brandMeta: document.querySelector('#brandMeta'),
  packageCount: document.querySelector('#packageCount'),
  runningCount: document.querySelector('#runningCount'),
  attentionCount: document.querySelector('#attentionCount'),
  searchInput: document.querySelector('#searchInput'),
  refreshButton: document.querySelector('#refreshButton'),
  installButton: document.querySelector('#installButton'),
  installDialog: document.querySelector('#installDialog'),
  installForm: document.querySelector('#installForm'),
  dropZone: document.querySelector('#dropZone'),
  packageFile: document.querySelector('#packageFile'),
  selectedFile: document.querySelector('#selectedFile'),
  selectedFileName: document.querySelector('#selectedFileName'),
  selectedFileSize: document.querySelector('#selectedFileSize'),
  clearFileButton: document.querySelector('#clearFileButton'),
  confirmInstallButton: document.querySelector('#confirmInstallButton'),
  installError: document.querySelector('#installError'),
  uploadHint: document.querySelector('#uploadHint'),
  removeDialog: document.querySelector('#removeDialog'),
  removePackageName: document.querySelector('#removePackageName'),
  purgeDataInput: document.querySelector('#purgeDataInput'),
  removeError: document.querySelector('#removeError'),
  confirmRemoveButton: document.querySelector('#confirmRemoveButton'),
  rpcDialog: document.querySelector('#rpcDialog'),
  rpcPackageName: document.querySelector('#rpcPackageName'),
  rpcTarget: document.querySelector('#rpcTarget'),
  rpcTimeout: document.querySelector('#rpcTimeout'),
  rpcArguments: document.querySelector('#rpcArguments'),
  rpcError: document.querySelector('#rpcError'),
  rpcResult: document.querySelector('#rpcResult'),
  rpcResultOutput: document.querySelector('#rpcResultOutput'),
  copyRpcResult: document.querySelector('#copyRpcResult'),
  confirmRpcButton: document.querySelector('#confirmRpcButton'),
  packageGrid: document.querySelector('#packageGrid'),
  emptyState: document.querySelector('#emptyState'),
  packageTemplate: document.querySelector('#packageTemplate'),
  notice: document.querySelector('#notice'),
  lastUpdated: document.querySelector('#lastUpdated'),
  activityUpdated: document.querySelector('#activityUpdated'),
  activityNote: document.querySelector('#activityNote'),
  eventList: document.querySelector('#eventList'),
  eventEmpty: document.querySelector('#eventEmpty'),
  eventLevelFilter: document.querySelector('#eventLevelFilter'),
  eventCategoryFilter: document.querySelector('#eventCategoryFilter'),
  eventPackageFilter: document.querySelector('#eventPackageFilter'),
  pauseEventsButton: document.querySelector('#pauseEventsButton'),
  clearEventsButton: document.querySelector('#clearEventsButton'),
};

const state = {
  authSession: null,
  packages: [],
  busy: new Set(),
  systemInfo: null,
  events: [],
  eventCursor: 0,
  eventJournalId: null,
  eventRevision: 0,
  eventsPaused: false,
  eventsLoading: false,
  appLoaded: false,
};
const maxVisibleEvents = 200;
let selectedPackageFile = null;
let packagePendingRemoval = null;
let packagePendingCall = null;
let eventPollTimer = null;

const stateMeta = {
  running: ['运行中', 'good'],
  starting: ['启动中', 'warn'],
  stopping: ['停止中', 'warn'],
  stuck: ['无响应', 'bad'],
  failed: ['失败', 'bad'],
  disabled: ['已停止', 'neutral'],
};

const eventLevelMeta = {
  info: '信息',
  warning: '警告',
  error: '错误',
};

const eventCategoryMeta = {
  daemon: '守护进程',
  package: '软件包',
  lifecycle: '生命周期',
  rpc: 'RPC',
  status: '状态',
};

const eventMessageMeta = {
  'daemon.started': 'circud 已启动',
  'daemon.stopping': 'circud 正在停止',
  'package.installed': '软件包已安装',
  'package.upgraded': '软件包已升级',
  'package.install_failed': '软件包安装失败',
  'package.removed': '软件包已卸载，数据已保留',
  'package.purged': '软件包及其数据已清除',
  'package.remove_failed': '软件包卸载失败',
  'lifecycle.started': '软件包已启动',
  'lifecycle.start_failed': '软件包运行时启动失败',
  'lifecycle.stopped': '软件包已停止',
  'lifecycle.reloaded': '软件包已重载',
  'lifecycle.reload_failed': '软件包运行时重载失败',
  'status.status_stale': '软件包状态快照已过期',
  'status.status_recovered': '软件包状态采集已恢复',
  'status.status_stuck': '软件包连续未响应状态检查',
  'rpc.not_found': 'RPC 目标软件包未运行',
  'rpc.unavailable': 'RPC 调用暂不可用',
  'rpc.package_calls_disabled': '软件包拒绝了跨包调用',
  'rpc.unhandled_call': 'RPC target 未被处理',
  'rpc.overloaded': 'RPC 队列已满',
  'rpc.timeout': 'RPC 调用超时',
  'rpc.cancelled': 'RPC 调用已取消',
  'rpc.remote_error': '软件包 RPC 返回错误',
};

function endpoint(path) {
  return new URL(path.replace(/^\//, ''), document.baseURI);
}

class RequestError extends Error {
  constructor(message, { code = '', status = 0 } = {}) {
    super(message);
    this.name = 'RequestError';
    this.code = code;
    this.status = status;
  }
}

async function readEnvelope(response, fallback) {
  let envelope;
  try {
    envelope = await response.json();
  } catch {
    throw new RequestError(
      `服务器返回了无法解析的响应（HTTP ${response.status}）`,
      { status: response.status });
  }
  if (!response.ok || envelope?.ok !== true) {
    throw new RequestError(
      envelope?.error?.message || `${fallback}（HTTP ${response.status}）`,
      { code: envelope?.error?.code || '', status: response.status });
  }
  if (!Object.prototype.hasOwnProperty.call(envelope, 'result')) {
    throw new RequestError('服务器返回了不完整的成功响应。', {
      status: response.status,
    });
  }
  return envelope.result;
}

async function publicFetch(path, options) {
  try {
    return await fetch(endpoint(path), {
      ...options,
      credentials: 'same-origin',
    });
  } catch {
    throw new RequestError('无法连接到 circud，请检查服务是否正在运行。');
  }
}

function csrfHeaders(headers = {}) {
  if (isAuthenticationDisabledSession(state.authSession)) {
    return { ...headers };
  }
  if (isAuthSession(state.authSession)) {
    return { ...headers, 'X-Circud-CSRF': state.authSession.csrfToken };
  }
  throw new RequestError('登录状态已失效，请重新登录。', {
    code: 'authentication_required', status: 401,
  });
}

function closeManagementDialogs() {
  for (const dialog of [elements.installDialog, elements.removeDialog,
                        elements.rpcDialog]) {
    if (dialog.open) dialog.close();
  }
}

function resetManagementState() {
  state.packages = [];
  state.systemInfo = null;
  state.events = [];
  state.eventCursor = 0;
  state.eventJournalId = null;
  state.eventRevision += 1;
  state.eventsLoading = false;
  state.busy.clear();
  state.appLoaded = false;
  if (eventPollTimer !== null) {
    clearInterval(eventPollTimer);
    eventPollTimer = null;
  }
  selectedPackageFile = null;
  packagePendingRemoval = null;
  packagePendingCall = null;
  closeManagementDialogs();
}

function showLogin(message = '请输入管理员账户继续。') {
  state.authSession = null;
  resetManagementState();
  elements.managementApp.hidden = true;
  elements.authView.hidden = false;
  elements.loginForm.hidden = false;
  elements.authRetryButton.hidden = true;
  elements.authStatus.textContent = message;
  elements.account.hidden = true;
  elements.accountName.textContent = '';
  elements.loginPassword.value = '';
  elements.loginError.hidden = true;
  elements.loginError.textContent = '';
  setConnection('offline', '需要登录');
  requestAnimationFrame(() => elements.loginUsername.focus());
}

function showAuthFailure(message) {
  state.authSession = null;
  resetManagementState();
  elements.managementApp.hidden = true;
  elements.authView.hidden = false;
  elements.loginForm.hidden = true;
  elements.authRetryButton.hidden = false;
  elements.authStatus.textContent = message;
  elements.account.hidden = true;
  elements.loginPassword.value = '';
  setConnection('offline', '无法连接');
}

function showManagement(session) {
  state.authSession = session;
  elements.authView.hidden = true;
  elements.managementApp.hidden = false;
  if (isAuthSession(session)) {
    elements.accountName.textContent = session.username;
    elements.account.hidden = false;
  } else {
    elements.accountName.textContent = '';
    elements.account.hidden = true;
  }
  setConnection('loading', '正在载入');
}

function isAuthenticationDisabledSession(value) {
  return value?.authEnabled === false && value.authenticated === false;
}

function isAnonymousSession(value) {
  return value?.authEnabled === true && value.authenticated === false;
}

function isAuthSession(value) {
  return value?.authEnabled === true && value.authenticated === true &&
    typeof value.username === 'string' && value.username.length !== 0 &&
    typeof value.csrfToken === 'string' && value.csrfToken.length !== 0 &&
    Number.isFinite(value.expiresAt);
}

function handleProtectedFailure(error) {
  if (error instanceof RequestError && error.status === 401 &&
      state.authSession?.authEnabled === true) {
    showLogin('登录已过期，请重新登录。');
  }
  return error;
}

async function protectedFetch(path, options) {
  const session = state.authSession;
  let response;
  try {
    response = await fetch(endpoint(path), {
      ...options,
      credentials: 'same-origin',
      headers: csrfHeaders(options?.headers),
    });
  } catch (error) {
    if (error instanceof RequestError) throw handleProtectedFailure(error);
    throw new RequestError('无法连接到 circud，请检查服务是否正在运行。');
  }
  if (state.authSession !== session) {
    throw new RequestError('登录状态已更改，已忽略旧请求。', {
      code: 'session_changed',
    });
  }
  if (response.status === 401) {
    const authenticationEnabled = session?.authEnabled === true;
    const error = new RequestError(authenticationEnabled
      ? '登录已过期，请重新登录。'
      : '服务器拒绝了请求（HTTP 401）。', {
      code: authenticationEnabled ? 'authentication_required' : 'unauthorized',
      status: 401,
    });
    handleProtectedFailure(error);
    throw error;
  }
  return response;
}

async function rpc(method, params = {}) {
  const response = await protectedFetch('rpc', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ method, params }),
  });
  try {
    return await readEnvelope(response, '请求失败');
  } catch (error) {
    throw handleProtectedFailure(error);
  }
}

function setConnection(status, text) {
  elements.connection.dataset.state = status;
  elements.connectionText.textContent = text;
}

function showNotice(message = '', tone = 'error') {
  elements.notice.hidden = !message;
  elements.notice.textContent = message;
  elements.notice.dataset.tone = tone;
}

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 ** 2) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
}

function formatEventTime(timestampMs) {
  const date = new Date(timestampMs);
  if (Number.isNaN(date.getTime())) return '时间未知';
  return new Intl.DateTimeFormat('zh-CN', {
    month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit',
    second: '2-digit', hour12: false,
  }).format(date);
}

function eventDescription(event) {
  return eventMessageMeta[`${event.category}.${event.code}`] ||
    event.message || event.code || '管理事件';
}

function eventDetails(event) {
  const details = [];
  if (event.details?.version) details.push(`版本 ${event.details.version}`);
  if (event.details?.origin) {
    details.push(event.details.origin === 'web' ? '来自 Web' : '来自软件包');
  }
  return details.join(' · ');
}

function renderEvents() {
  elements.eventList.replaceChildren();
  elements.eventEmpty.hidden = state.events.length !== 0;
  for (const event of [...state.events].reverse()) {
    const row = document.createElement('li');
    row.className = 'event-row';
    row.dataset.level = event.level || 'info';

    const marker = document.createElement('span');
    marker.className = 'event-marker';
    marker.setAttribute('aria-hidden', 'true');

    const body = document.createElement('div');
    body.className = 'event-body';
    const heading = document.createElement('div');
    heading.className = 'event-row-heading';
    const message = document.createElement('strong');
    message.textContent = eventDescription(event);
    const time = document.createElement('time');
    time.dateTime = new Date(event.timestampMs).toISOString();
    time.textContent = formatEventTime(event.timestampMs);
    heading.append(message, time);

    const meta = document.createElement('p');
    const values = [
      eventLevelMeta[event.level] || event.level,
      eventCategoryMeta[event.category] || event.category,
      event.package,
      eventDetails(event),
    ].filter(Boolean);
    meta.textContent = values.join(' · ');
    body.append(heading, meta);
    row.append(marker, body);
    elements.eventList.append(row);
  }
}

function setActivityNote(message = '', tone = '') {
  elements.activityNote.hidden = !message;
  elements.activityNote.textContent = message;
  elements.activityNote.dataset.tone = tone;
}

function eventQuery() {
  const params = { afterId: state.eventCursor, limit: 200 };
  if (elements.eventLevelFilter.value) params.level = elements.eventLevelFilter.value;
  if (elements.eventCategoryFilter.value) params.category = elements.eventCategoryFilter.value;
  if (elements.eventPackageFilter.value) params.package = elements.eventPackageFilter.value;
  return params;
}

async function loadEvents() {
  if (!state.authSession || state.eventsLoading || state.eventsPaused ||
      state.systemInfo?.events?.enabled === false) return;
  state.eventsLoading = true;
  const revision = state.eventRevision;
  try {
    let rounds = 0;
    let page;
    let gap = false;
    let reset = false;
    do {
      const query = eventQuery();
      page = await rpc('events.list', query);
      if (revision !== state.eventRevision) return;
      if (state.eventJournalId && page.journalId !== state.eventJournalId) {
        state.events = [];
        state.eventCursor = 0;
        state.eventJournalId = page.journalId;
        reset = true;
        rounds += 1;
        if (query.afterId !== 0 && rounds < 4) continue;
      } else {
        state.eventJournalId = page.journalId;
      }
      if (page.reset) {
        reset = true;
        state.events = [];
      }
      if (page.gap) {
        gap = true;
        state.events = [];
      }
      state.events.push(...page.events);
      if (state.events.length > maxVisibleEvents) {
        state.events.splice(0, state.events.length - maxVisibleEvents);
      }
      state.eventCursor = page.nextAfterId;
      rounds += 1;
    } while (page.hasMore && rounds < 4);
    if (gap) {
      setActivityNote('较早的活动已从内存缓冲区淘汰，列表从当前最早记录继续。', 'warning');
    } else if (reset) {
      setActivityNote('服务端事件游标已重置，可能是 circud 刚刚重启。', 'warning');
    } else {
      setActivityNote();
    }
    elements.activityUpdated.textContent = state.eventsPaused
      ? '已暂停'
      : `更新于 ${new Intl.DateTimeFormat('zh-CN', {
          hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
        }).format(new Date())}`;
    renderEvents();
  } catch (error) {
    if (!(error instanceof RequestError && error.code === 'session_changed')) {
      setActivityNote(error instanceof Error ? error.message : String(error), 'error');
    }
  } finally {
    state.eventsLoading = false;
    if (state.authSession && revision !== state.eventRevision &&
        !state.eventsPaused) {
      queueMicrotask(loadEvents);
    }
  }
}

function resetEventFilter() {
  state.eventRevision += 1;
  state.eventCursor = 0;
  state.events = [];
  setActivityNote();
  renderEvents();
  loadEvents();
}

function updateEventPackageFilter() {
  const selected = elements.eventPackageFilter.value;
  elements.eventPackageFilter.replaceChildren(new Option('全部', ''));
  for (const pkg of state.packages) {
    elements.eventPackageFilter.add(new Option(pkg.name || pkg.id, pkg.id));
  }
  if (selected && ![...elements.eventPackageFilter.options]
      .some((option) => option.value === selected)) {
    elements.eventPackageFilter.add(new Option(`${selected}（已卸载）`, selected));
  }
  if (selected) {
    elements.eventPackageFilter.value = selected;
  }
}

async function loadSystemInfo() {
  try {
    state.systemInfo = await rpc('system.info');
    if (state.systemInfo?.version) {
      elements.brandMeta.textContent = `Linux 管理框架 · v${state.systemInfo.version}`;
    }
    const uploadBytes = state.systemInfo?.limits?.uploadBytes;
    if (Number.isFinite(uploadBytes)) {
      elements.uploadHint.textContent = `服务端允许最大 ${formatBytes(uploadBytes)}`;
    } else {
      elements.uploadHint.textContent = '上传大小由服务端限制';
    }
  } catch {
    elements.uploadHint.textContent = '无法读取上传限制，服务端仍会校验';
  }
}

function setInstallFile(file) {
  selectedPackageFile = file || null;
  elements.installError.hidden = true;
  elements.installError.textContent = '';
  elements.selectedFile.hidden = !file;
  elements.dropZone.hidden = Boolean(file);
  elements.confirmInstallButton.disabled = !file;
  if (file) {
    elements.selectedFileName.textContent = file.name;
    elements.selectedFileSize.textContent = formatBytes(file.size);
    const uploadLimit = state.systemInfo?.limits?.uploadBytes;
    if (!file.name.toLocaleLowerCase().endsWith('.zip')) {
      elements.installError.textContent = '请选择 .zip 软件包文件。';
      elements.installError.hidden = false;
      elements.confirmInstallButton.disabled = true;
    } else if (Number.isFinite(uploadLimit) && file.size > uploadLimit) {
      elements.installError.textContent =
        `文件大小为 ${formatBytes(file.size)}，超过服务端限制 ${formatBytes(uploadLimit)}。`;
      elements.installError.hidden = false;
      elements.confirmInstallButton.disabled = true;
    }
  } else {
    elements.packageFile.value = '';
  }
}

function openInstallDialog() {
  setInstallFile(null);
  elements.installDialog.showModal();
}

async function installSelectedPackage() {
  if (!selectedPackageFile) return;
  const file = selectedPackageFile;
  elements.confirmInstallButton.disabled = true;
  elements.clearFileButton.disabled = true;
  elements.installButton.disabled = true;
  elements.confirmInstallButton.textContent = '正在安装…';
  elements.installError.hidden = true;
  try {
    const response = await protectedFetch('api/packages/install', {
      method: 'POST',
      headers: { 'content-type': 'application/zip' },
      body: file,
    });
    const installed = await readEnvelope(response, '安装失败');
    elements.installDialog.close();
    await loadPackages({ quiet: true });
    showNotice(`已安装 ${installed.name || installed.id} ${installed.version}`, 'success');
  } catch (error) {
    handleProtectedFailure(error);
    if (!elements.installDialog.open) return;
    elements.installError.textContent = error instanceof Error ? error.message : String(error);
    elements.installError.hidden = false;
  } finally {
    elements.confirmInstallButton.textContent = '上传并安装';
    elements.confirmInstallButton.disabled = !selectedPackageFile;
    elements.clearFileButton.disabled = false;
    elements.installButton.disabled = false;
  }
}

function openRemoveDialog(packageId) {
  const pkg = state.packages.find((item) => item.id === packageId);
  if (!pkg) return;
  packagePendingRemoval = pkg;
  elements.removePackageName.textContent = pkg.name || pkg.id;
  elements.purgeDataInput.checked = false;
  elements.removeError.hidden = true;
  elements.removeError.textContent = '';
  elements.removeDialog.showModal();
}

async function removeSelectedPackage() {
  if (!packagePendingRemoval) return;
  const pkg = packagePendingRemoval;
  state.busy.add(pkg.id);
  elements.confirmRemoveButton.disabled = true;
  elements.confirmRemoveButton.textContent = '正在卸载…';
  elements.removeError.hidden = true;
  try {
    await rpc('package.remove', {
      id: pkg.id,
      purgeData: elements.purgeDataInput.checked,
    });
    elements.removeDialog.close();
    await loadPackages({ quiet: true });
    showNotice(`已卸载 ${pkg.name || pkg.id}`, 'success');
  } catch (error) {
    elements.removeError.textContent = error instanceof Error ? error.message : String(error);
    elements.removeError.hidden = false;
  } finally {
    state.busy.delete(pkg.id);
    elements.confirmRemoveButton.disabled = false;
    elements.confirmRemoveButton.textContent = '确认卸载';
    render();
  }
}

function openRpcDialog(packageId) {
  const pkg = state.packages.find((item) => item.id === packageId);
  if (!pkg) return;
  packagePendingCall = pkg;
  elements.rpcPackageName.textContent = pkg.name || pkg.id;
  elements.rpcTarget.value = '';
  elements.rpcArguments.value = '{}';
  elements.rpcTimeout.value = String(
    state.systemInfo?.limits?.defaultCallTimeoutMs || 30000);
  elements.rpcTimeout.max = String(
    state.systemInfo?.limits?.maxCallTimeoutMs || 300000);
  elements.rpcError.hidden = true;
  elements.rpcResult.hidden = true;
  elements.rpcResultOutput.textContent = '';
  elements.rpcDialog.showModal();
  requestAnimationFrame(() => elements.rpcTarget.focus());
}

async function runPackageCall() {
  if (!packagePendingCall) return;
  const target = elements.rpcTarget.value.trim();
  if (!target) {
    elements.rpcError.textContent = 'Target 不能为空。';
    elements.rpcError.hidden = false;
    return;
  }
  let args;
  try {
    args = JSON.parse(elements.rpcArguments.value);
  } catch (error) {
    elements.rpcError.textContent = `Arguments 不是有效 JSON：${error.message}`;
    elements.rpcError.hidden = false;
    return;
  }
  const timeoutMs = Number(elements.rpcTimeout.value);
  const maxTimeout = state.systemInfo?.limits?.maxCallTimeoutMs || 300000;
  if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > maxTimeout) {
    elements.rpcError.textContent = `超时必须是 1 到 ${maxTimeout} 之间的整数。`;
    elements.rpcError.hidden = false;
    return;
  }

  elements.confirmRpcButton.disabled = true;
  elements.confirmRpcButton.textContent = '调用中…';
  elements.rpcError.hidden = true;
  elements.rpcResult.hidden = true;
  try {
    const result = await rpc('call', {
      package: packagePendingCall.id,
      target,
      arguments: args,
      timeoutMs,
    });
    if (result.ok !== true) {
      const code = result?.error?.code ? `[${result.error.code}] ` : '';
      throw new Error(`${code}${result?.error?.message || '软件包调用失败'}`);
    }
    elements.rpcResultOutput.textContent = JSON.stringify(result.value, null, 2);
    elements.rpcResult.hidden = false;
  } catch (error) {
    elements.rpcError.textContent = error instanceof Error ? error.message : String(error);
    elements.rpcError.hidden = false;
  } finally {
    elements.confirmRpcButton.disabled = false;
    elements.confirmRpcButton.textContent = '发送调用';
  }
}

function packageInitial(pkg) {
  const source = pkg.name || pkg.id || '?';
  return source.trim().charAt(0).toLocaleUpperCase() || '?';
}

function render() {
  const query = elements.searchInput.value.trim().toLocaleLowerCase();
  const filtered = state.packages.filter((pkg) =>
    `${pkg.name || ''} ${pkg.id || ''}`.toLocaleLowerCase().includes(query));
  const running = state.packages.filter((pkg) => pkg.runtimeState === 'running').length;
  const attention = state.packages.filter((pkg) => ['failed', 'stuck'].includes(pkg.runtimeState)).length;
  elements.packageCount.textContent = String(state.packages.length);
  elements.runningCount.textContent = String(running);
  elements.attentionCount.textContent = String(attention);
  elements.packageGrid.replaceChildren();
  elements.emptyState.hidden = filtered.length !== 0;

  for (const pkg of filtered) {
    const fragment = elements.packageTemplate.content.cloneNode(true);
    const card = fragment.querySelector('.package-card');
    const [label, tone] = stateMeta[pkg.runtimeState] || [pkg.runtimeState || '未知', 'neutral'];
    card.dataset.packageId = pkg.id;
    fragment.querySelector('.package-icon').textContent = packageInitial(pkg);
    fragment.querySelector('h3').textContent = pkg.name || pkg.id;
    fragment.querySelector('.package-id').textContent = pkg.id;
    fragment.querySelector('.version').textContent = pkg.version || '—';
    fragment.querySelector('.runtime-state').textContent = label;
    const badge = fragment.querySelector('.badge');
    badge.textContent = label;
    badge.dataset.tone = tone;
    const snapshot = pkg.status;
    const statusMessage = snapshot?.value?.message ||
      (snapshot?.value?.state ? `状态快照：${snapshot.value.state}${snapshot.stale ? '（已过期）' : ''}` : '');
    fragment.querySelector('.runtime-message').textContent = pkg.runtimeError || statusMessage ||
      (pkg.enabled ? '运行时已启用，等待首次状态采集。' : '软件包当前未运行。');
    const busy = state.busy.has(pkg.id);
    for (const button of fragment.querySelectorAll('button')) button.disabled = busy;
    fragment.querySelector('[data-action="start"]').hidden = pkg.runtimeState === 'running';
    fragment.querySelector('[data-action="stop"]').hidden = pkg.runtimeState === 'disabled';
    elements.packageGrid.append(fragment);
  }
}

async function loadPackages({ quiet = false } = {}) {
  elements.refreshButton.disabled = true;
  if (!quiet) showNotice();
  try {
    state.packages = await rpc('packages.list');
    updateEventPackageFilter();
    setConnection('online', '服务已连接');
    elements.lastUpdated.textContent = `更新于 ${new Intl.DateTimeFormat('zh-CN', {
      hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
    }).format(new Date())}`;
    render();
  } catch (error) {
    if (error instanceof RequestError &&
        (error.status === 401 || error.code === 'session_changed')) return;
    setConnection('offline', '连接失败');
    showNotice(error instanceof Error ? error.message : String(error));
  } finally {
    elements.refreshButton.disabled = false;
  }
}

async function runAction(packageId, action) {
  const methods = { start: 'package.start', stop: 'package.stop', reload: 'package.reload' };
  state.busy.add(packageId);
  render();
  showNotice();
  try {
    if (action === 'status') {
      await rpc('status.get', { id: packageId, refresh: true });
    } else {
      await rpc(methods[action], { id: packageId });
    }
    await loadPackages({ quiet: true });
  } catch (error) {
    showNotice(error instanceof Error ? error.message : String(error));
  } finally {
    state.busy.delete(packageId);
    render();
  }
}

elements.refreshButton.addEventListener('click', () => loadPackages());
elements.installButton.addEventListener('click', openInstallDialog);
elements.packageFile.addEventListener('change', () => {
  setInstallFile(elements.packageFile.files?.[0] || null);
});
elements.clearFileButton.addEventListener('click', () => setInstallFile(null));
elements.confirmInstallButton.addEventListener('click', installSelectedPackage);
elements.confirmRemoveButton.addEventListener('click', removeSelectedPackage);
elements.confirmRpcButton.addEventListener('click', runPackageCall);
elements.copyRpcResult.addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(elements.rpcResultOutput.textContent);
    elements.copyRpcResult.textContent = '已复制';
    setTimeout(() => { elements.copyRpcResult.textContent = '复制'; }, 1200);
  } catch {
    elements.rpcError.textContent = '浏览器不允许访问剪贴板。';
    elements.rpcError.hidden = false;
  }
});
for (const eventName of ['dragenter', 'dragover']) {
  elements.dropZone.addEventListener(eventName, (event) => {
    event.preventDefault();
    elements.dropZone.dataset.dragging = 'true';
  });
}
for (const eventName of ['dragleave', 'drop']) {
  elements.dropZone.addEventListener(eventName, (event) => {
    event.preventDefault();
    delete elements.dropZone.dataset.dragging;
  });
}
elements.dropZone.addEventListener('drop', (event) => {
  const file = event.dataTransfer?.files?.[0];
  if (file) setInstallFile(file);
});
elements.installDialog.addEventListener('close', () => setInstallFile(null));
elements.removeDialog.addEventListener('close', () => {
  packagePendingRemoval = null;
  elements.purgeDataInput.checked = false;
});
elements.rpcDialog.addEventListener('close', () => {
  packagePendingCall = null;
  elements.rpcResult.hidden = true;
});
elements.searchInput.addEventListener('input', render);
for (const filter of [elements.eventLevelFilter, elements.eventCategoryFilter,
                      elements.eventPackageFilter]) {
  filter.addEventListener('change', resetEventFilter);
}
elements.pauseEventsButton.addEventListener('click', () => {
  state.eventsPaused = !state.eventsPaused;
  elements.pauseEventsButton.textContent = state.eventsPaused ? '继续' : '暂停';
  elements.activityUpdated.textContent = state.eventsPaused ? '已暂停' : '正在更新';
  if (!state.eventsPaused) loadEvents();
});
elements.clearEventsButton.addEventListener('click', () => {
  state.eventRevision += 1;
  state.events = [];
  setActivityNote();
  renderEvents();
});
elements.packageGrid.addEventListener('click', (event) => {
  const button = event.target.closest('button[data-action]');
  const card = event.target.closest('[data-package-id]');
  if (!button || !card || button.disabled) return;
  if (button.dataset.action === 'remove') {
    openRemoveDialog(card.dataset.packageId);
    return;
  }
  if (button.dataset.action === 'call') {
    openRpcDialog(card.dataset.packageId);
    return;
  }
  runAction(card.dataset.packageId, button.dataset.action);
});

async function startManagement() {
  if (state.appLoaded || !state.authSession) return;
  state.appLoaded = true;
  await Promise.all([loadSystemInfo(), loadPackages()]);
  if (!state.authSession) return;
  if (state.systemInfo?.events?.enabled === false) {
    elements.activityUpdated.textContent = '事件日志已禁用';
    elements.pauseEventsButton.disabled = true;
    elements.clearEventsButton.disabled = true;
    for (const filter of [elements.eventLevelFilter, elements.eventCategoryFilter,
                          elements.eventPackageFilter]) filter.disabled = true;
    setActivityNote('服务端配置已关闭管理事件日志。');
    return;
  }
  await loadEvents();
  eventPollTimer = setInterval(() => {
    if (!document.hidden && state.authSession) loadEvents();
  }, 3000);
}

async function loadAuthSession() {
  elements.authView.hidden = false;
  elements.managementApp.hidden = true;
  elements.loginForm.hidden = true;
  elements.authRetryButton.hidden = true;
  elements.authStatus.textContent = '正在确认当前登录状态…';
  setConnection('loading', '正在检查会话');
  try {
    const response = await publicFetch('api/auth/session', {
      method: 'GET',
      headers: { accept: 'application/json' },
      cache: 'no-store',
    });
    const session = await readEnvelope(response, '无法读取登录状态');
    if (isAuthenticationDisabledSession(session)) {
      showManagement(session);
      await startManagement();
      return;
    }
    if (isAnonymousSession(session)) {
      showLogin();
      return;
    }
    if (!isAuthSession(session)) {
      throw new RequestError('服务器返回了无效的登录状态。');
    }
    showManagement(session);
    await startManagement();
  } catch (error) {
    showAuthFailure(error instanceof Error ? error.message : String(error));
  }
}

async function login(event) {
  event.preventDefault();
  const username = elements.loginUsername.value.trim();
  const password = elements.loginPassword.value;
  if (!username || !password) {
    elements.loginError.textContent = '请输入用户名和密码。';
    elements.loginError.hidden = false;
    return;
  }

  elements.loginButton.disabled = true;
  elements.loginButton.textContent = '正在登录…';
  elements.loginError.hidden = true;
  setConnection('loading', '正在登录');
  try {
    const response = await publicFetch('api/auth/login', {
      method: 'POST',
      headers: {
        accept: 'application/json',
        'content-type': 'application/json',
      },
      body: JSON.stringify({ username, password }),
    });
    elements.loginPassword.value = '';
    const session = await readEnvelope(response, '登录失败');
    if (!isAuthSession(session)) {
      throw new RequestError('服务器返回了无效的登录状态。');
    }
    showManagement(session);
    await startManagement();
  } catch (error) {
    elements.loginPassword.value = '';
    elements.loginError.textContent = error instanceof RequestError &&
      (error.status === 401 || error.code === 'invalid_credentials')
      ? '用户名或密码不正确。'
      : error instanceof Error ? error.message : String(error);
    elements.loginError.hidden = false;
    setConnection('offline', '登录失败');
    requestAnimationFrame(() => elements.loginPassword.focus());
  } finally {
    elements.loginButton.disabled = false;
    elements.loginButton.textContent = '登录';
  }
}

async function logout() {
  if (isAuthenticationDisabledSession(state.authSession)) return;
  if (!isAuthSession(state.authSession)) return showLogin();
  elements.logoutButton.disabled = true;
  setConnection('loading', '正在退出');
  try {
    const response = await protectedFetch('api/auth/logout', {
      method: 'POST',
    });
    if (response.status !== 204) {
      await readEnvelope(response, '退出失败');
      throw new RequestError(`退出响应无效（HTTP ${response.status}）`, {
        status: response.status,
      });
    }
    showLogin('已安全退出。');
  } catch (error) {
    if (!(error instanceof RequestError && error.status === 401)) {
      showNotice(error instanceof Error ? error.message : String(error));
      setConnection('online', '服务已连接');
    }
  } finally {
    elements.logoutButton.disabled = false;
  }
}

elements.loginForm.addEventListener('submit', login);
elements.authRetryButton.addEventListener('click', loadAuthSession);
elements.logoutButton.addEventListener('click', logout);

loadAuthSession();
