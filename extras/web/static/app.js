"use strict";

// The server owns the engine. This file only renders snapshots and sends actions.
// Prices are integer cents and amounts are integer strings: keep arithmetic exact.
const $ = (id) => document.getElementById(id);
const MAX_VALUE = 1_000_000_000_000n;
const PAGE_SIZE = 25;
const timeFormat = new Intl.DateTimeFormat("pt-BR", {
  hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false,
});
const eventLabels = {
  created: "Nova ordem", amended: "Alteração", cancelled: "Cancelamento",
  trade: "Execução", repriced: "Pegged", error: "Erro", system: "Sistema",
};
const eventIcons = {
  created: "+", amended: "↗", cancelled: "×", trade: "⇄",
  repriced: "↕", error: "!", system: "·",
};
const statusLabels = { active: "Aberta", filled: "Executada", cancelled: "Cancelada" };
const typeLabels = { limit: "Limit", market: "Market", peg: "Pegged" };

let state = null;
let online = false;
let busy = false;
let polling = false;
let requestSequence = 0;
let lastAppliedRequest = 0;
let orderType = "limit";
let orderSide = "buy";
let bookMode = "levels";
let orderPage = 0;
let amendDraft = null;
let cancelId = null;
let limitDraft = null;
let notificationTimer;
let lastConnectionProblem = "";

function escapeHTML(value) {
  return String(value ?? "").replace(/[&<>"']/g, (character) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[character]);
}

function integer(value) { return BigInt(value ?? "0"); }
function quantity(value) { return integer(value).toLocaleString("pt-BR"); }
function price(value) {
  if (value == null) return "—";
  const cents = integer(value);
  const absolute = cents < 0n ? -cents : cents;
  return `${cents < 0n ? "−" : ""}${quantity(absolute / 100n)},${String(absolute % 100n).padStart(2, "0")}`;
}
function decimalPrice(value) {
  const cents = integer(value);
  return `${cents / 100n}.${String(cents % 100n).padStart(2, "0")}`;
}
function currency(value) { return value == null ? "—" : `R$ ${price(value)}`; }
function percentage(part, total) {
  return total === 0n ? 0 : Number((part * 10000n) / total) / 100;
}
function time(value) {
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? "—" : timeFormat.format(date);
}
function sideLabel(side) { return side === "buy" ? "Compra" : "Venda"; }
function sumLevels(levels) {
  return levels.reduce((sum, level) => sum + integer(level.total_quantity), 0n);
}

function parseQuantity(input) {
  const value = input.trim();
  if (!/^\d+$/.test(value)) throw new Error("Informe uma quantidade inteira, sem separadores.");
  const amount = integer(value);
  if (amount < 1n || amount > MAX_VALUE) {
    throw new Error(`A quantidade deve estar entre 1 e ${quantity(MAX_VALUE)}.`);
  }
  return String(amount);
}

function parseOrderId(input) {
  const match = /^#?(\d+)$/.exec(input.trim());
  if (!match || integer(match[1]) < 1n) {
    throw new Error("Informe um ID válido, como 12 ou #12.");
  }
  return String(integer(match[1]));
}

function parsePrice(input) {
  const value = input.trim().replace(",", ".");
  if (!/^\d+(?:\.\d{1,2})?$/.test(value)) {
    throw new Error("Informe um preço com até 2 casas decimais, sem separador de milhar.");
  }
  const [whole, fraction = ""] = value.split(".");
  const cents = integer(whole) * 100n + integer(fraction.padEnd(2, "0"));
  if (cents < 1n || cents > MAX_VALUE) {
    throw new Error(`O preço deve estar entre R$ 0,01 e ${currency(MAX_VALUE)}.`);
  }
  return decimalPrice(cents);
}

function validatedInput(input, parser) {
  try {
    input.setCustomValidity("");
    return parser(input.value);
  } catch (error) {
    input.setCustomValidity(error.message);
    input.reportValidity();
    throw error;
  }
}

function notify(message, kind = "success", canRecover = false) {
  clearTimeout(notificationTimer);
  $("notification-text").textContent = message;
  $("notification").dataset.kind = kind;
  $("notification").dataset.connection = "false";
  $("notification").hidden = false;
  $("recover-button").hidden = !canRecover;
  if (kind !== "error") notificationTimer = setTimeout(() => { $("notification").hidden = true; }, 6500);
}

function setConnection(connected, connecting = false, engineUnavailable = false) {
  online = connected;
  $("connection").dataset.state = connected ? "online" : connecting ? "connecting" : "offline";
  $("connection-text").textContent = connected ? "Conectado" : connecting ? "Conectando" : engineUnavailable ? "Engine indisponível" : "Sem conexão";
  if (connected && state) {
    $("session-detail").textContent = `Sessão ${String(state.session_id).slice(0, 8)} · revisão ${quantity(state.revision)}`;
    $("recover-button").hidden = true;
    if (lastConnectionProblem && $("notification").dataset.connection === "true") $("notification").hidden = true;
    lastConnectionProblem = "";
  }
  syncControls();
}

function syncControls() {
  document.querySelectorAll("[data-mutation]").forEach((button) => {
    button.disabled = busy || !online || !state;
  });
  // Reset starts a fresh bridge, so it must remain usable after bridge failure.
  for (const id of ["reset-button", "confirm-reset", "recover-button"]) {
    $(id).disabled = busy || !state;
  }
  $("reset-button").title = state && !online ? "Reiniciar a sessão para reconectar à engine" : "Reiniciar sessão";
  const demoAvailable = state && state.metrics.submitted === 0;
  $("demo-button").disabled = busy || !online || !demoAvailable;
  $("demo-button").title = demoAvailable ? "Adicionar ordens ilustrativas à sessão vazia" : "Disponível antes da primeira ordem. Reinicie a sessão para usar a demonstração.";
  $("submit-label").textContent = busy ? "Processando…" : orderSide === "buy" ? "Enviar compra" : "Enviar venda";
  $("order-form").setAttribute("aria-busy", String(busy));
  $("limit-dialog").querySelectorAll("[data-close-dialog]").forEach((button) => {
    button.disabled = busy;
  });
}

async function fetchJSON(path, body) {
  const sequence = ++requestSequence;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);
  try {
    const response = await fetch(path, {
      method: body === undefined ? "GET" : "POST",
      headers: body === undefined ? { Accept: "application/json" } : {
        Accept: "application/json", "Content-Type": "application/json",
        ...(state ? { "X-Session-ID": state.session_id } : {}),
      },
      body: body === undefined ? undefined : JSON.stringify(body),
      cache: "no-store",
      signal: controller.signal,
    });
    const data = await response.json();
    return { data, sequence, status: response.status, success: response.ok && data.ok !== false };
  } finally {
    clearTimeout(timeout);
  }
}

function applyState(data, sequence) {
  if (!data.book || !data.metrics || sequence < lastAppliedRequest) return;
  lastAppliedRequest = sequence;
  const sameSession = state && data.session_id === state.session_id;
  if (sameSession && data.revision < state.revision) return;
  const changed = !sameSession || data.revision !== state?.revision;
  if (state && !sameSession) {
    // Order IDs are reused after reset. Never keep an old dialog targeting one.
    for (const id of ["amend-dialog", "cancel-dialog", "reset-dialog", "limit-dialog"]) {
      if ($(id).open) $(id).close();
    }
    amendDraft = null;
    cancelId = null;
    limitDraft = null;
    $("manage-order-id").value = "";
    $("manage-order-id").setCustomValidity("");
    $("manage-order-error").hidden = true;
  }
  state = data;
  if (!sameSession) orderPage = 0;
  if (changed) renderState();
  syncControls();
}

async function pollState() {
  if (busy || polling) return;
  polling = true;
  try {
    const result = await fetchJSON("/api/state");
    applyState(result.data, result.sequence);
    if (!result.success) {
      reportConnectionProblem(result.data.error || "A engine está indisponível.", result.status === 503);
      return;
    }
    setConnection(true);
  } catch (error) {
    reportConnectionProblem("Sem resposta do servidor. A reconexão é automática.");
  } finally {
    polling = false;
  }
}

function reportConnectionProblem(message, engineUnavailable = false) {
  setConnection(false, false, engineUnavailable);
  $("recover-button").hidden = !state;
  $("session-detail").textContent = engineUnavailable ? "Último livro recebido · reinicie a sessão para continuar" : "Conexão indisponível · tentando novamente";
  if (lastConnectionProblem !== message) {
    // Preserve a command's uncertain-outcome warning until the user reads it.
    const commandError = !$("notification").hidden && $("notification").dataset.kind === "error" && $("notification").dataset.connection !== "true";
    if (!commandError) {
      notify(message, "error", Boolean(state));
      $("notification").dataset.connection = "true";
    }
    lastConnectionProblem = message;
  }
}

async function mutate(path, command, message) {
  if (busy || !state || (!online && path !== "/api/reset")) return false;
  busy = true;
  syncControls();
  try {
    const result = await fetchJSON(path, command);
    applyState(result.data, result.sequence);
    setConnection(result.status !== 503, false, result.status === 503);
    if (result.data.confirmation_required && command.action === "limit") {
      openLimitConfirmation(command, result.data.confirmation_required);
      return false;
    }
    if (!result.success) {
      notify(result.data.error || "Não foi possível concluir a operação.", "error", !online && Boolean(state));
      return false;
    }
    notify(typeof message === "function" ? message(result.data) : message);
    return true;
  } catch (error) {
    setConnection(false);
    notify("A resposta do servidor não chegou. Confira o livro e o Status após a reconexão antes de reenviar a ordem.", "error", Boolean(state));
    return false;
  } finally {
    busy = false;
    syncControls();
  }
}

function openLimitConfirmation(command, quote) {
  // Freeze the command being confirmed; never rebuild it from mutable form inputs.
  limitDraft = { ...command, confirmation: quote.token };
  $("limit-description").textContent =
    `${sideLabel(quote.side)} limit de ${quantity(quote.quantity)} unidades a ${currency(quote.limit_price)}.`;
  $("limit-best-price").textContent =
    `A melhor ${quote.side === "buy" ? "venda (ask)" : "compra (bid)"} disponível é ${currency(quote.best_price)}.`;
  $("limit-changed").hidden = !quote.changed;
  $("limit-error").hidden = true;
  if (!$("limit-dialog").open) $("limit-dialog").showModal();
  // A price refresh requires a deliberate new choice, never a focused submit button.
  // mutate's finally first re-enables the controls after receiving the preview.
  queueMicrotask(() => {
    if ($("limit-dialog").open) $("decline-limit").focus();
  });
}

function renderState() {
  const { buys, sells } = state.book;
  const bid = buys[0];
  const ask = sells[0];
  $("best-bid").textContent = bid ? currency(bid.price) : "—";
  $("best-ask").textContent = ask ? currency(ask.price) : "—";
  $("best-bid-detail").textContent = bid ? `${quantity(bid.total_quantity)} unidades no melhor preço` : "Nenhuma compra no livro";
  $("best-ask-detail").textContent = ask ? `${quantity(ask.total_quantity)} unidades no melhor preço` : "Nenhuma venda no livro";
  $("spread").textContent = bid && ask ? currency(integer(ask.price) - integer(bid.price)) : "—";
  $("trade-count").textContent = quantity(state.metrics.trade_count);
  $("executed-quantity").textContent = `${quantity(state.metrics.executed_quantity)} unidades negociadas`;
  $("event-count").textContent = quantity(state.events.length);
  $("open-order-count").textContent = `${quantity(state.metrics.open_orders)} abertas`;
  $("submitted-count").textContent = quantity(state.metrics.submitted);
  $("status-open-count").textContent = quantity(state.metrics.open_orders);
  $("cancelled-count").textContent = quantity(state.metrics.cancelled_orders);
  $("status-executed-quantity").textContent = quantity(state.metrics.executed_quantity);
  $("session-detail").textContent = `Sessão ${String(state.session_id).slice(0, 8)} · revisão ${quantity(state.revision)}`;
  renderBook();
  renderTrades();
  renderOrders();
  renderEvents();
  updateEstimate();
}

function renderBook() {
  if (!state) return;
  const { buys, sells } = state.book;
  const byOrder = bookMode === "orders";
  const toDisplay = (levels) => {
    if (byOrder) {
      // The snapshot already follows price/time priority, including pegged
      // repricing. Flatten it without sorting by ID or combining equal prices.
      return levels.flatMap((level) => level.orders.map((order) => ({
        ...order, displayQuantity: integer(order.remaining_quantity),
      })));
    }
    let running = 0n;
    return levels.map((level) => {
      running += integer(level.total_quantity);
      return { ...level, displayQuantity: bookMode === "cumulative" ? running : integer(level.total_quantity) };
    });
  };
  const bids = toDisplay(buys);
  const asks = toDisplay(sells);
  const maximum = [...bids, ...asks].reduce((max, level) => level.displayQuantity > max ? level.displayQuantity : max, 0n);
  const cell = (level, side, index) => {
    if (!level) return `<span class="book-cell missing ${side}" aria-hidden="true"></span>`;
    if (byOrder) {
      const title = `Ordem #${level.id} · ${sideLabel(level.side)} ${typeLabels[level.type]} · ${quantity(level.displayQuantity)} unidades a ${currency(level.price)}. Clique para usar este preço.`;
      return `<button type="button" class="book-cell book-order ${side}" data-book-order-id="${escapeHTML(level.id)}" data-book-price="${escapeHTML(level.price)}" title="${escapeHTML(title)}" aria-label="${escapeHTML(title)}"><span class="order-quote"><span class="order-quantity">${quantity(level.displayQuantity)}</span> <span class="order-at">@</span> <span class="order-price">${price(level.price)}</span></span></button>`;
    }
    const amount = `<span class="level-quantity">${quantity(level.displayQuantity)}</span>`;
    const value = `<span class="level-price">${price(level.price)}</span>`;
    const title = `${side === "bid" ? "Compra" : "Venda"}: ${quantity(level.total_quantity)} unidades a ${currency(level.price)}. ${level.orders.length} ordem(ns). Clique para usar este preço.`;
    return `<button type="button" class="book-cell ${side}${index === 0 ? " best" : ""}" style="--depth:${percentage(level.displayQuantity, maximum)}%" data-book-price="${escapeHTML(level.price)}" title="${escapeHTML(title)}" aria-label="${escapeHTML(title)}">${side === "bid" ? amount + value : value + amount}</button>`;
  };
  if (!bids.length && !asks.length) {
    const description = state.metrics.submitted ? "Envie uma ordem limit para adicionar liquidez." : "Envie uma ordem limit ou carregue a demonstração.";
    $("book-content").innerHTML = `<div class="empty-state"><strong>Livro vazio</strong><p>${description}</p></div>`;
  } else {
    $("book-content").innerHTML = Array.from({ length: Math.max(bids.length, asks.length) }, (_, index) => `<div class="book-row">${cell(bids[index], "bid", index)}${cell(asks[index], "ask", index)}</div>`).join("");
  }
  const bidTotal = sumLevels(buys);
  const askTotal = sumLevels(sells);
  const total = bidTotal + askTotal;
  $("buy-liquidity").textContent = quantity(bidTotal);
  $("sell-liquidity").textContent = quantity(askTotal);
  $("buy-liquidity-bar").style.width = `${percentage(bidTotal, total)}%`;
  $("sell-liquidity-bar").style.width = `${percentage(askTotal, total)}%`;
  $("book-content").dataset.mode = bookMode;
  $("book-content").setAttribute("aria-label", byOrder ? "Ordens em prioridade de preço e chegada" : "Níveis do livro de ofertas");
  $("book-column-headings").hidden = byOrder;
  $("book-level-count").textContent = byOrder
    ? `${quantity(bids.length + asks.length)} ordens no livro`
    : `${quantity(buys.length + sells.length)} faixas de preço`;
  document.querySelectorAll(".book-quantity-label").forEach((label) => {
    label.textContent = bookMode === "cumulative" ? "Qtd. acumulada" : "Quantidade";
  });
  $("book-caption").textContent = byOrder
    ? "print book · uma ordem por linha · quantidade restante @ preço (R$)"
    : bookMode === "cumulative" ? "Quantidade somada a partir do melhor preço · mesma escala" : "Quantidade em cada preço · barras na mesma escala";
}

function renderTrades() {
  if (!state.trades.length) {
    $("trades-content").innerHTML = '<div class="empty-state"><strong>Nenhuma execução</strong><p>As execuções aparecem quando houver ordens compatíveis.</p></div>';
    return;
  }
  $("trades-content").innerHTML = state.trades.map((trade) => {
    const side = trade.side === "buy" ? "buy" : "sell";
    const description = `${sideLabel(side)} agressora #${trade.aggressive_order_id} · ordem passiva #${trade.resting_order_id}`;
    return `<div class="trade-row" title="${escapeHTML(description)}"><span class="trade-price ${side}-text"><span aria-label="${sideLabel(side)}">${side === "buy" ? "↗" : "↘"}</span><span>${price(trade.price)}</span></span><span class="trade-quantity">${quantity(trade.quantity)}</span><time class="trade-time" datetime="${escapeHTML(trade.time)}">${escapeHTML(time(trade.time))}</time></div>`;
  }).join("");
}

function filteredOrders() {
  const status = $("order-status-filter").value;
  const side = $("order-side-filter").value;
  const priority = new Map();
  const active = [];
  for (const levels of [state.book.buys, state.book.sells]) {
    for (const level of levels) {
      level.orders.forEach((order, index) => {
        priority.set(order.id, index + 1);
        active.push(order);
      });
    }
  }
  const orders = status === "active" ? active : state.orders.filter((order) => status === "all" || order.status === status);
  return { orders: orders.filter((order) => side === "all" || order.side === side), priority };
}

function renderOrders() {
  if (!state) return;
  const { orders, priority } = filteredOrders();
  const pageCount = Math.max(1, Math.ceil(orders.length / PAGE_SIZE));
  orderPage = Math.min(orderPage, pageCount - 1);
  const offset = orderPage * PAGE_SIZE;
  const visible = orders.slice(offset, offset + PAGE_SIZE);
  $("orders-subtitle").textContent = $("order-status-filter").value === "active" ? "Por lado, melhor preço e prioridade na faixa" : "Mais recentes primeiro · quantidade restante após execução ou cancelamento";
  $("orders-body").innerHTML = visible.length ? visible.map((order) => {
    const side = order.side === "buy" ? "buy" : "sell";
    const type = typeLabels[order.type] || order.type;
    const peg = order.type === "peg" ? `<span class="type-reference">${escapeHTML(order.peg_reference)}</span>` : "";
    const actions = order.status === "active" ? `<div class="order-actions"><button type="button" class="small-button" data-order-action="amend" data-order-id="${escapeHTML(order.id)}" data-mutation aria-label="Alterar ordem ${escapeHTML(order.id)}">Alterar</button><button type="button" class="small-button cancel-action" data-order-action="cancel" data-order-id="${escapeHTML(order.id)}" data-mutation aria-label="Cancelar ordem ${escapeHTML(order.id)}">Cancelar</button></div>` : '<span class="order-actions">—</span>';
    return `<tr data-order-row="${escapeHTML(order.id)}"><td class="mono order-id">#${escapeHTML(order.id)}</td><td><span class="${side}-text">${sideLabel(side)}</span></td><td><span class="type-badge ${order.type === "peg" ? "peg" : ""}">${escapeHTML(type)}</span>${peg}</td><td class="numeric mono">${price(order.price)}</td><td class="numeric mono" title="Quantidade original: ${quantity(order.original_quantity)}">${quantity(order.remaining_quantity)}</td><td class="numeric mono" title="Posição na fila do mesmo lado e preço. Sequência: ${escapeHTML(order.sequence)}">${priority.has(order.id) ? `#${priority.get(order.id)}` : "—"}</td><td><span class="status-label ${escapeHTML(order.status)}">${escapeHTML(statusLabels[order.status] || order.status)}</span></td><td>${actions}</td></tr>`;
  }).join("") : '<tr><td colspan="8" class="empty-table">Nenhuma ordem para este filtro.</td></tr>';
  $("orders-pagination-label").textContent = orders.length ? `${quantity(offset + 1)}–${quantity(offset + visible.length)} de ${quantity(orders.length)} ordens` : "Nenhuma ordem";
  $("orders-previous").disabled = orderPage === 0;
  $("orders-next").disabled = orderPage >= pageCount - 1;
  syncControls();
}

function renderEvents() {
  if (!state) return;
  const kind = $("event-filter").value;
  const search = $("event-search").value.trim().toLocaleLowerCase("pt-BR");
  const events = state.events.filter((event) => {
    const matchingKind = kind === "all" || (kind === "orders" ? ["created", "amended", "cancelled"].includes(event.kind) : event.kind === kind);
    const matchingSearch = !search || `${event.message} ${event.order_id || ""} ${eventLabels[event.kind] || event.kind}`.toLocaleLowerCase("pt-BR").includes(search);
    return matchingKind && matchingSearch;
  });
  $("events-content").innerHTML = events.length ? events.map((event) => `<li class="event-row" data-kind="${escapeHTML(event.kind)}"><span class="event-icon" aria-hidden="true">${eventIcons[event.kind] || "·"}</span><span class="event-kind">${escapeHTML(eventLabels[event.kind] || event.kind)}</span><span class="event-message">${escapeHTML(event.message)}</span><time class="event-time" datetime="${escapeHTML(event.time)}" title="${escapeHTML(new Date(event.time).toLocaleString("pt-BR"))}">${escapeHTML(time(event.time))}</time></li>`).join("") : `<li class="empty-state"><strong>${state.events.length ? "Nenhum evento para este filtro" : "Sem atividade"}</strong><p>${state.events.length ? "Tente outro tipo de evento ou altere a busca." : "As operações desta sessão serão registradas aqui."}</p></li>`;
  $("events-footer").textContent = `${quantity(events.length)} eventos exibidos · até 300 eventos recentes · mais recentes primeiro`;
}

function selectOrderType(type) {
  orderType = type;
  document.querySelectorAll("[data-order-type]").forEach((button) => {
    const selected = button.dataset.orderType === type;
    button.classList.toggle("selected", selected);
    button.setAttribute("aria-pressed", String(selected));
  });
  $("price-field").hidden = type !== "limit";
  $("order-price").required = type === "limit";
  $("peg-reference").hidden = type !== "peg";
  $("order-price").setCustomValidity("");
  updateEntryDescription();
}

function selectOrderSide(side) {
  orderSide = side;
  document.querySelectorAll("[data-order-side]").forEach((button) => {
    const selected = button.dataset.orderSide === side;
    button.classList.toggle("selected", selected);
    button.setAttribute("aria-pressed", String(selected));
  });
  $("submit-order").classList.toggle("buy-submit", side === "buy");
  $("submit-order").classList.toggle("sell-submit", side === "sell");
  updateEntryDescription();
  syncControls();
}

function updateEntryDescription() {
  $("peg-reference-label").textContent = orderSide === "buy" ? "Melhor bid · compra" : "Melhor offer · venda";
  $("peg-reference-description").textContent = `Segue o melhor preço de ${orderSide === "buy" ? "compra" : "venda"} das ordens limit.`;
  $("order-rule").textContent = orderType === "market" ? "Executa no melhor preço disponível. O saldo sem liquidez é cancelado." : orderType === "peg" ? "A referência acompanha as limits. Sem referência, a ordem é rejeitada ou cancelada." : "Pede confirmação se cruzar o livro. O saldo restante entra no livro.";
  updateEstimate();
}

function updateEstimate() {
  $("estimate-label").textContent = orderType === "limit" ? "Valor na ordem" : orderType === "peg" ? "Referência atual" : "Liquidez disponível";
  let label = "—";
  if (orderType === "market" && state) {
    label = `${quantity(sumLevels(orderSide === "buy" ? state.book.sells : state.book.buys))} unidades`;
  } else if (orderType === "peg" && state) {
    const levels = orderSide === "buy" ? state.book.buys : state.book.sells;
    // Pegs are excluded from their own reference. Search for the best limit.
    const reference = levels.find((level) => level.orders.some((order) => order.type === "limit"));
    label = reference ? currency(reference.price) : "Sem referência";
  } else if (orderType === "limit") {
    try {
      const amount = integer(parseQuantity($("order-quantity").value));
      const [whole, fraction] = parsePrice($("order-price").value).split(".");
      label = currency((integer(whole) * 100n + integer(fraction)) * amount);
    } catch (error) { /* An unfinished input has no estimate. */ }
  }
  $("order-estimate").textContent = label;
}

function openAmend(id) {
  const order = state.orders.find((candidate) => candidate.id === id && candidate.status === "active");
  if (!order) return notify("Esta ordem já não está aberta. Confira o Status.", "error");
  amendDraft = { id, type: order.type, price: order.price == null ? "" : decimalPrice(order.price), quantity: order.remaining_quantity };
  $("amend-title").textContent = `Alterar ordem #${id}`;
  $("amend-description").textContent = `${sideLabel(order.side)} ${typeLabels[order.type]} · ${quantity(order.remaining_quantity)} unidades a ${currency(order.price)}`;
  $("amend-price").value = amendDraft.price.replace(".", ",");
  $("amend-quantity").value = amendDraft.quantity;
  $("amend-price-field").hidden = order.type === "peg";
  $("amend-price").required = order.type !== "peg";
  $("amend-note").textContent = order.type === "peg" ? "O preço segue a referência e não pode ser alterado manualmente. Aumentar a quantidade reposiciona a ordem na fila." : "Alterar o preço ou aumentar a quantidade reposiciona a ordem na fila. A alteração pode gerar trades.";
  $("amend-price").setCustomValidity("");
  $("amend-quantity").setCustomValidity("");
  $("amend-error").hidden = true;
  $("amend-dialog").showModal();
  (order.type === "peg" ? $("amend-quantity") : $("amend-price")).focus();
}

function openCancel(id) {
  const order = state.orders.find((candidate) => candidate.id === id && candidate.status === "active");
  if (!order) return notify("Esta ordem já não está aberta. Confira o Status.", "error");
  cancelId = id;
  $("cancel-title").textContent = `Cancelar ordem #${id}?`;
  $("cancel-description").textContent = `${sideLabel(order.side)} ${typeLabels[order.type]} · ${quantity(order.remaining_quantity)} unidades a ${currency(order.price)}`;
  $("cancel-error").hidden = true;
  $("cancel-dialog").showModal();
}

function switchTab(name, focus = false) {
  for (const tabName of ["desk", "status"]) {
    const selected = tabName === name;
    const tab = $(`${tabName}-tab`);
    tab.classList.toggle("active", selected);
    tab.setAttribute("aria-selected", String(selected));
    tab.tabIndex = selected ? 0 : -1;
    $(`${tabName}-panel`).hidden = !selected;
    if (selected && focus) tab.focus();
  }
}

document.querySelectorAll("[data-order-type]").forEach((button) => button.addEventListener("click", () => selectOrderType(button.dataset.orderType)));
document.querySelectorAll("[data-order-side]").forEach((button) => button.addEventListener("click", () => selectOrderSide(button.dataset.orderSide)));
document.querySelectorAll("[data-book-mode]").forEach((button) => button.addEventListener("click", () => {
  bookMode = button.dataset.bookMode;
  document.querySelectorAll("[data-book-mode]").forEach((option) => {
    const selected = option === button;
    option.classList.toggle("selected", selected);
    option.setAttribute("aria-pressed", String(selected));
  });
  renderBook();
}));

for (const id of ["order-price", "order-quantity", "amend-price", "amend-quantity"]) {
  $(id).addEventListener("input", () => {
    $(id).setCustomValidity("");
    if (id.startsWith("order-")) updateEstimate();
  });
}

$("order-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (busy || !online) return;
  try {
    const command = { action: orderType, side: orderSide };
    if (orderType === "limit") command.price = validatedInput($("order-price"), parsePrice);
    command.quantity = validatedInput($("order-quantity"), parseQuantity);
    if (orderType === "peg") command.reference = orderSide === "buy" ? "bid" : "offer";
    await mutate("/api/command", command, "Ordem processada. Confira o livro, as execuções e o Status.");
  } catch (error) { /* The invalid field displays its specific error. */ }
});

$("limit-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!limitDraft || busy || !online) return;
  const draft = limitDraft;
  $("limit-error").hidden = true;
  if (await mutate("/api/command", draft, "Limit confirmada e processada. Confira as execuções e o saldo no livro.")) {
    $("limit-dialog").close();
  } else if (limitDraft === draft && $("limit-dialog").open) {
    $("limit-error").textContent = $("notification-text").textContent;
    $("limit-error").hidden = false;
  }
});
$("limit-dialog").addEventListener("close", () => { limitDraft = null; });
$("limit-dialog").addEventListener("cancel", (event) => {
  // Once Confirm is pressed the request cannot be withdrawn by closing the UI.
  if (busy) event.preventDefault();
});

$("book-content").addEventListener("click", (event) => {
  const button = event.target.closest("[data-book-price]");
  if (!button) return;
  selectOrderType("limit");
  $("order-price").value = decimalPrice(button.dataset.bookPrice).replace(".", ",");
  updateEstimate();
  $("order-price").focus({ preventScroll: window.innerWidth > 680 });
});

$("orders-body").addEventListener("click", (event) => {
  const button = event.target.closest("[data-order-action]");
  if (!button || button.disabled) return;
  if (button.dataset.orderAction === "amend") openAmend(button.dataset.orderId);
  if (button.dataset.orderAction === "cancel") openCancel(button.dataset.orderId);
});

$("manage-order-form").addEventListener("submit", (event) => {
  event.preventDefault();
  if (busy || !online || !state) return;
  const input = $("manage-order-id");
  const errorMessage = $("manage-order-error");
  errorMessage.hidden = true;
  try {
    const id = validatedInput(input, parseOrderId);
    // Look through the session, not the currently filtered/paginated table.
    const order = state.orders.find((candidate) => candidate.id === id);
    if (!order) throw new Error(`A ordem #${id} não existe nesta sessão.`);
    if (order.status !== "active") throw new Error(`A ordem #${id} não está aberta (${(statusLabels[order.status] || order.status).toLocaleLowerCase("pt-BR")}).`);
    if (event.submitter?.value === "cancel") openCancel(id);
    else openAmend(id);
  } catch (error) {
    errorMessage.textContent = error.message;
    errorMessage.hidden = false;
    input.focus();
  }
});
$("manage-order-id").addEventListener("input", () => {
  $("manage-order-id").setCustomValidity("");
  $("manage-order-error").hidden = true;
});

$("amend-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!amendDraft || busy || !online) return;
  $("amend-error").hidden = true;
  try {
    const command = { action: "amend", id: amendDraft.id };
    if (amendDraft.type !== "peg") {
      const nextPrice = validatedInput($("amend-price"), parsePrice);
      if (nextPrice !== amendDraft.price) command.price = nextPrice;
    }
    const nextQuantity = validatedInput($("amend-quantity"), parseQuantity);
    if (nextQuantity !== amendDraft.quantity) command.quantity = nextQuantity;
    if (!command.price && !command.quantity) {
      $("amend-error").textContent = "Altere o preço ou a quantidade antes de confirmar.";
      $("amend-error").hidden = false;
      return;
    }
    if (await mutate("/api/command", command, `Ordem #${amendDraft.id} alterada.`)) {
      $("amend-dialog").close();
    } else {
      $("amend-error").textContent = $("notification-text").textContent;
      $("amend-error").hidden = false;
    }
  } catch (error) { /* The invalid field displays its specific error. */ }
});

$("cancel-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!cancelId) return;
  $("cancel-error").hidden = true;
  if (await mutate("/api/command", { action: "cancel", id: cancelId }, `Ordem #${cancelId} cancelada.`)) {
    $("cancel-dialog").close();
  } else {
    $("cancel-error").textContent = $("notification-text").textContent;
    $("cancel-error").hidden = false;
  }
});

$("demo-button").addEventListener("click", () => mutate("/api/demo", {}, "Demonstração carregada. As ordens são ilustrativas e podem ser alteradas ou canceladas."));
function openReset() {
  $("reset-error").hidden = true;
  $("reset-dialog").showModal();
}
$("reset-button").addEventListener("click", openReset);
$("recover-button").addEventListener("click", openReset);
$("reset-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  $("reset-error").hidden = true;
  if (await mutate("/api/reset", {}, "Nova sessão iniciada. O livro está vazio.")) {
    $("reset-dialog").close();
  } else {
    $("reset-error").textContent = $("notification-text").textContent;
    $("reset-error").hidden = false;
  }
});
document.querySelectorAll("[data-close-dialog]").forEach((button) => button.addEventListener("click", () => $(button.dataset.closeDialog).close()));
$("dismiss-notification").addEventListener("click", () => { $("notification").hidden = true; });

for (const name of ["desk", "status"]) {
  $(`${name}-tab`).addEventListener("click", () => switchTab(name));
  $(`${name}-tab`).addEventListener("keydown", (event) => {
    if (["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) {
      event.preventDefault();
      switchTab(event.key === "Home" ? "desk" : event.key === "End" ? "status" : name === "desk" ? "status" : "desk", true);
    }
  });
}
for (const id of ["order-status-filter", "order-side-filter"]) {
  $(id).addEventListener("change", () => { orderPage = 0; renderOrders(); });
}
$("orders-previous").addEventListener("click", () => { orderPage -= 1; renderOrders(); });
$("orders-next").addEventListener("click", () => { orderPage += 1; renderOrders(); });
$("event-filter").addEventListener("change", renderEvents);
$("event-search").addEventListener("input", renderEvents);

setConnection(false, true);
pollState();
setInterval(pollState, 1000);
