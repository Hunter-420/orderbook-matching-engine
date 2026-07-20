// app.js - UI interaction and simulation loop

let engine = null;
let currentSide = 'B';
let tapeLines = [];
let simInterval = null;
let simSpeed = 600;
let simMidPrice = 10000; // $100.00
const INVALID = -1;

function init() {
    engine = new Engine(uiCallback);
    updateAllVisualizers();
    populateHeroTicker();
}

function setSide(side) {
    currentSide = side;
    document.getElementById('buyBtn').classList.remove('active', 'buy-active');
    document.getElementById('sellBtn').classList.remove('active', 'sell-active');
    
    if (side === 'B') {
        document.getElementById('buyBtn').classList.add('active', 'buy-active');
    } else {
        document.getElementById('sellBtn').classList.add('active', 'sell-active');
    }
}

function placeOrder() {
    const price = parseFloat(document.getElementById('priceInput').value);
    const qty = parseInt(document.getElementById('qtyInput').value);
    
    if (isNaN(price) || isNaN(qty) || qty <= 0 || price <= 0) return;
    
    const priceCents = Math.round(price * 100);
    engine.newOrder(currentSide, priceCents, qty);
    
    updateAllVisualizers();
}

function cancelAll() {
    engine.cancelAll();
    updateAllVisualizers();
}

// --- Simulation Loop ---

function toggleSim() {
    const btn = document.getElementById('simBtn');
    if (simInterval) {
        clearInterval(simInterval);
        simInterval = null;
        btn.textContent = '▶ Start Market Sim';
        btn.classList.remove('active');
    } else {
        btn.textContent = '⏸ Stop Market Sim';
        btn.classList.add('active');
        runSimStep();
        simInterval = setInterval(runSimStep, simSpeed);
    }
}

function updateSpeed(val) {
    simSpeed = parseInt(val);
    document.getElementById('speedLabel').textContent = simSpeed + 'ms';
    if (simInterval) {
        clearInterval(simInterval);
        simInterval = setInterval(runSimStep, simSpeed);
    }
}

function runSimStep() {
    // Random walk the mid price
    simMidPrice += (Math.random() - 0.5) * 50; 
    if (simMidPrice < 100) simMidPrice = 100;
    
    // Generate order around mid
    const side = Math.random() > 0.5 ? 'B' : 'S';
    let offset = Math.floor(Math.random() * 20) * 25; // 0, 25, 50, 75, etc
    if (side === 'B') offset = -offset; // Bids below mid
    
    let price = simMidPrice + offset;
    
    // 10% chance to cross the spread aggressively
    if (Math.random() > 0.9) {
        price += (side === 'B' ? 50 : -50); 
    }
    
    const qty = (Math.floor(Math.random() * 10) + 1) * 10;
    
    engine.newOrder(side, price, qty);
    updateAllVisualizers();
    
    // Randomly cancel an old order to keep memory clean
    if (Math.random() > 0.7 && engine.orderDir.size > 0) {
        const ids = Array.from(engine.orderDir.keys());
        const randomId = ids[Math.floor(Math.random() * ids.length)];
        engine.cancelOrder(randomId);
        updateAllVisualizers();
    }
}

// --- UI Callbacks & Visualizers ---

function uiCallback(event) {
    let msg = '';
    let clz = '';
    const time = new Date().toISOString().substring(11,23);
    
    if (event.type === 'ACCEPTED') {
        const sideStr = event.side === 'B' ? 'BUY ' : 'SELL';
        msg = `<span class="tape-type">ACK</span> Order ${event.id} | ${sideStr} ${event.qty} @ $${(event.price/100).toFixed(2)}`;
        clz = 'accepted';
    } else if (event.type === 'FILL') {
        msg = `<span class="tape-type tape-fill">FILL</span> Order ${event.incId} x ${event.resId} | ${event.qty} @ $${(event.price/100).toFixed(2)}`;
        clz = 'fill';
    } else if (event.type === 'CANCELLED') {
        msg = `<span class="tape-type">CXL</span> Order ${event.id} cancelled`;
        clz = 'cancelled';
    }
    
    tapeLines.unshift({ msg, clz, time });
    if (tapeLines.length > 50) tapeLines.pop();
    renderTape();
}

function renderTape() {
    const tape = document.getElementById('execTape');
    tape.innerHTML = tapeLines.map(t => 
        `<div class="tape-entry ${t.clz}">
            <div>${t.msg}</div>
            <div class="tape-time">${t.time}</div>
         </div>`
    ).join('');
}

function updateAllVisualizers() {
    renderBook();
    renderMemory();
    renderNodes();
}

function renderBook() {
    const snap = engine.getOrderbookSnapshot();
    const ladder = document.getElementById('bookLadder');
    let html = '';
    
    // Asks (reverse order so lowest ask is at bottom of ask section)
    for (let i = snap.asks.length - 1; i >= 0; i--) {
        const a = snap.asks[i];
        html += `<div class="book-row">
            <div class="book-bg" style="right: 50%; width: ${Math.min(a.qty/5, 48)}%; background: rgba(248,113,113,0.15)"></div>
            <div class="bid-qty empty">-</div>
            <div class="price">$${(a.price/100).toFixed(2)}</div>
            <div class="ask-qty">${a.qty}</div>
        </div>`;
    }
    
    if (snap.asks.length > 0 && snap.bids.length > 0) {
        const spread = (snap.asks[0].price - snap.bids[0].price) / 100;
        document.getElementById('spreadInfo').textContent = `Spread: $${spread.toFixed(2)}`;
    } else {
        document.getElementById('spreadInfo').textContent = `Spread: —`;
    }
    
    // Bids (highest bid at top of bid section)
    for (let b of snap.bids) {
        html += `<div class="book-row">
            <div class="book-bg" style="left: 50%; width: ${Math.min(b.qty/5, 48)}%; background: rgba(34,211,165,0.15)"></div>
            <div class="bid-qty">${b.qty}</div>
            <div class="price">$${(b.price/100).toFixed(2)}</div>
            <div class="ask-qty empty">-</div>
        </div>`;
    }
    
    ladder.innerHTML = html;
}

function renderMemory() {
    const snap = engine.getMemorySnapshot();
    
    document.getElementById('activeCount').textContent = snap.activeCount;
    document.getElementById('freeCount').textContent = (snap.capacity - snap.activeCount).toLocaleString();
    document.getElementById('nextFreeIdx').textContent = snap.nextFree;
    
    const pct = ((snap.activeCount / 1000) * 100).toFixed(1); // Scale for visual effect since 1M is huge
    document.getElementById('usedPct').textContent = `${(snap.activeCount / snap.capacity * 100).toFixed(3)}%`;
    document.getElementById('memBarFill').style.width = Math.min(Math.max(pct, 1), 100) + '%';
    
    // Free list visualization (first 5 nodes)
    let flHtml = '';
    let curr = snap.nextFree;
    for (let i = 0; i < 5; i++) {
        if (curr === INVALID) break;
        flHtml += `<div class="fl-node">[${curr}]</div>`;
        if (i < 4) flHtml += `<div class="fl-arrow">→</div>`;
        curr = engine.pool.get(curr).next; // Cheating a bit by hitting engine directly for the demo
    }
    flHtml += `<div class="fl-arrow">→ ...</div>`;
    document.getElementById('freelistViz').innerHTML = flHtml;
    
    // Slots grid
    let slotsHtml = snap.usedSlots.map(s => `<div class="slot-chip">Slot [${s}]</div>`).join('');
    if (!slotsHtml) slotsHtml = '<div style="color:var(--text-3);font-size:0.8rem;padding:10px">No active slots</div>';
    document.getElementById('slotsGrid').innerHTML = slotsHtml;
}

function renderNodes() {
    const nodes = engine.getNodeSnapshot();
    const tbody = document.getElementById('nodeTableBody');
    const empty = document.getElementById('nodeEmpty');
    
    if (nodes.length === 0) {
        tbody.innerHTML = '';
        empty.style.display = 'block';
        document.getElementById('chainViz').innerHTML = '';
        return;
    }
    
    empty.style.display = 'none';
    
    let html = '';
    let chainsByPrice = {};
    
    nodes.forEach(n => {
        const sideClass = n.side === 'B' ? 'side-bid' : 'side-ask';
        const pStr = n.prev === INVALID ? '<span class="ptr-none">NONE</span>' : `<span class="ptr-val">${n.prev}</span>`;
        const nStr = n.next === INVALID ? '<span class="ptr-none">NONE</span>' : `<span class="ptr-val">${n.next}</span>`;
        
        html += `<tr>
            <td>[${n.slot}]</td>
            <td>${n.id}</td>
            <td class="${sideClass}">${n.side === 'B' ? 'BID' : 'ASK'}</td>
            <td>$${(n.price/100).toFixed(2)}</td>
            <td>${n.qty}</td>
            <td>${pStr}</td>
            <td>${nStr}</td>
        </tr>`;
        
        // Build chain viz logic
        if (!chainsByPrice[n.price]) chainsByPrice[n.price] = [];
        chainsByPrice[n.price].push(n);
    });
    
    tbody.innerHTML = html;
    
    // Chain visualization
    let chainHtml = '';
    for (let p in chainsByPrice) {
        let chain = chainsByPrice[p];
        if (chain.length > 1) { // Only show levels with multiple orders to demonstrate linked list
            // Sort by linked list traversal (simple approach for demo since we know the order they were pushed)
            chain.sort((a,b) => a.id - b.id); 
            
            chainHtml += `<div class="chain-group">
                <div class="chain-label">$${(p/100).toFixed(2)} Level Chain</div>
                <div class="chain-nodes">`;
                
            chain.forEach((node, idx) => {
                chainHtml += `<div class="chain-node">Slot ${node.slot} (Ord ${node.id})</div>`;
                if (idx < chain.length - 1) chainHtml += `<div class="chain-arrow">↔</div>`;
            });
                
            chainHtml += `</div></div>`;
        }
    }
    
    const vizDiv = document.getElementById('chainViz');
    if (chainHtml) {
        vizDiv.innerHTML = '<div style="width:100%;font-size:0.8rem;color:var(--text-3);margin-bottom:8px">Linked List Chains (Shared Price Levels):</div>' + chainHtml;
    } else {
        vizDiv.innerHTML = '';
    }
}

function populateHeroTicker() {
    const ticker = document.getElementById('heroTicker');
    let lines = [];
    let p = 10100;
    for(let i=0; i<15; i++) {
        const side = Math.random() > 0.5 ? 'B' : 'S';
        const qty = Math.floor(Math.random() * 10 + 1) * 10;
        p += (Math.random() - 0.5) * 10;
        const clz = side === 'B' ? 'fill-up' : 'fill-down';
        lines.push(`<div class="ticker-line ${clz}">FILL ${qty} shares @ $${(p/100).toFixed(2)}</div>`);
    }
    ticker.innerHTML = lines.join('');
    
    setInterval(() => {
        const side = Math.random() > 0.5 ? 'B' : 'S';
        const qty = Math.floor(Math.random() * 10 + 1) * 10;
        p += (Math.random() - 0.5) * 10;
        const clz = side === 'B' ? 'fill-up' : 'fill-down';
        const line = `<div class="ticker-line ${clz}">FILL ${qty} shares @ $${(p/100).toFixed(2)}</div>`;
        ticker.insertAdjacentHTML('afterbegin', line);
        if (ticker.children.length > 20) ticker.removeChild(ticker.lastChild);
    }, 800);
}

// Start
window.onload = init;
