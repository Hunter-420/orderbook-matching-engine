// app.js

let engine;
let currentSide = 'B';
let simInterval = null;
let simSpeed = 600;
let simMidPrice = 10000; // $100.00 in cents

// === View Switching ===
function switchTab(tabId) {
    document.getElementById('view-demo').className = (tabId === 'demo') ? 'view-active' : 'view-hidden';
    document.getElementById('view-docs').className = (tabId === 'docs') ? 'view-active' : 'view-hidden';
    window.scrollTo(0, 0);
}

// === Markdown Docs Logic ===
async function loadDoc(filename) {
    try {
        const response = await fetch(filename);
        if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
        const text = await response.text();
        
        // Render markdown
        document.getElementById('markdown-container').innerHTML = marked.parse(text);
        
        // Intercept internal links to other markdown files
        const links = document.getElementById('markdown-container').querySelectorAll('a');
        links.forEach(link => {
            const href = link.getAttribute('href');
            if (href && href.endsWith('.md')) {
                link.onclick = (e) => {
                    e.preventDefault();
                    loadDoc(href);
                };
            }
        });

        // Update active state in sidebar
        document.querySelectorAll('.docs-nav a').forEach(a => {
            a.classList.remove('active');
            if (a.getAttribute('onclick').includes(filename)) {
                a.classList.add('active');
            }
        });

    } catch (e) {
        document.getElementById('markdown-container').innerHTML = `<div style="color:red">Failed to load document: ${e.message}</div>`;
    }
}


// === Demo Logic ===

function setSide(side) {
    currentSide = side;
    document.getElementById('buyBtn').className = side === 'B' ? 'side-btn active' : 'side-btn';
    document.getElementById('sellBtn').className = side === 'S' ? 'side-btn active' : 'side-btn';
}

function placeOrder() {
    const price = parseFloat(document.getElementById('priceInput').value);
    const qty = parseInt(document.getElementById('qtyInput').value, 10);
    
    if (isNaN(price) || price <= 0 || isNaN(qty) || qty <= 0) return;
    
    // Convert to integer cents
    const priceCents = Math.round(price * 100);
    engine.newOrder(currentSide, priceCents, qty);
    
    updateAllVisualizers();
}

function cancelAll() {
    const ids = Array.from(engine.orderDir.keys());
    for (const id of ids) {
        engine.cancelOrder(id);
    }
    updateAllVisualizers();
}

function toggleSim() {
    const btn = document.getElementById('simBtn');
    if (simInterval) {
        clearInterval(simInterval);
        simInterval = null;
        btn.innerHTML = '▶ Start Market Sim';
        btn.classList.remove('active');
    } else {
        btn.innerHTML = '⏸ Stop Simulation';
        btn.classList.add('active');
        simInterval = setInterval(runSimStep, simSpeed);
    }
}

function updateSpeed(val) {
    simSpeed = parseInt(val, 10);
    document.getElementById('speedLabel').innerText = simSpeed + 'ms';
    if (simInterval) {
        clearInterval(simInterval);
        simInterval = setInterval(runSimStep, simSpeed);
    }
}

function runSimStep() {
    // Random walk the mid price
    simMidPrice += (Math.random() - 0.5) * 50; 
    
    const side = Math.random() > 0.5 ? 'B' : 'S';
    
    // Spread orders around the mid price
    // e.g., standard deviation of 20 cents
    let offset = (Math.random() + Math.random() + Math.random() - 1.5) * 40; 
    
    // Force a spread occasionally
    if (Math.random() > 0.9) {
        offset += (side === 'B' ? 50 : -50); 
    }
    
    let price = Math.round(simMidPrice + offset); // MUST BE INTEGER
    const qty = (Math.floor(Math.random() * 10) + 1) * 10;
    
    engine.newOrder(side, price, qty);
    updateAllVisualizers();
}

// === UI Callbacks ===

// Called by engine when a trade or action happens
function uiCallback(event) {
    const ticker = document.getElementById('execTape');
    let line = '';
    
    if (event.type === 'NEW') {
        const sClass = event.side === 'B' ? 't-buy' : 't-sell';
        line = `<div class="tape-entry"><span class="${sClass}">NEW ${event.side === 'B'?'BUY':'SELL'}</span> Order ${event.id}: ${event.qty} @ $${(event.price/100).toFixed(2)}</div>`;
    } else if (event.type === 'FILL') {
        line = `<div class="tape-entry t-fill">FILL</div> <span style="font-size:0.8rem; font-weight: 500; color: var(--text-main)">Matched ${event.incId} & ${event.resId} | ${event.qty} shares @ $${(event.price/100).toFixed(2)}</span>`;
    } else if (event.type === 'CANCEL') {
        line = `<div class="tape-entry" style="color:var(--text-light)">CANCEL Order ${event.id}</div>`;
    }
    
    if (line) {
        ticker.insertAdjacentHTML('afterbegin', line);
        if (ticker.children.length > 50) ticker.removeChild(ticker.lastChild);
    }
}

function populateHeroTicker() {
    const ticker = document.getElementById('heroTicker');
    const sides = ['B', 'S'];
    let localMid = 10100;
    
    for (let i = 0; i < 15; i++) {
        const side = sides[Math.floor(Math.random()*2)];
        const qty = (Math.floor(Math.random()*5)+1)*10;
        const p = localMid + Math.floor(Math.random()*20 - 10);
        const sClass = side === 'B' ? 't-buy' : 't-sell';
        const str = side === 'B' ? 'BUY ' : 'SELL';
        
        let html = `<div class="ticker-line"><span class="${sClass}">${str}</span> ${qty} @ $${(p/100).toFixed(2)}</div>`;
        if (Math.random() > 0.8) {
            html += `<div class="ticker-line t-fill">FILL ${qty} @ $${(p/100).toFixed(2)}</div>`;
        }
        ticker.insertAdjacentHTML('afterbegin', html);
    }
}

// === Visualizers ===

function updateAllVisualizers() {
    renderBook();
    renderMemory();
    renderNodes();
}

function renderBook() {
    const snap = engine.getOrderbookSnapshot();
    const ladder = document.getElementById('bookLadder');
    ladder.innerHTML = '';
    
    // Sort asks descending for display (highest ask at top)
    const asks = snap.asks.slice().reverse();
    
    let maxQty = 0;
    for (const a of snap.asks) if (a.qty > maxQty) maxQty = a.qty;
    for (const b of snap.bids) if (b.qty > maxQty) maxQty = b.qty;
    
    let bestAsk = null;
    let bestBid = null;

    asks.forEach(a => {
        bestAsk = a.price;
        const pct = (a.qty / maxQty) * 100;
        ladder.innerHTML += `
        <div class="book-row">
            <div class="bg-bar-sell" style="width: ${pct}%"></div>
            <span></span>
            <span class="price-col">$${(a.price/100).toFixed(2)}</span>
            <span class="sell-qty">${a.qty}</span>
        </div>`;
    });
    
    snap.bids.forEach(b => {
        if (!bestBid) bestBid = b.price;
        const pct = (b.qty / maxQty) * 100;
        ladder.innerHTML += `
        <div class="book-row">
            <div class="bg-bar-buy" style="width: ${pct}%"></div>
            <span class="buy-qty">${b.qty}</span>
            <span class="price-col">$${(b.price/100).toFixed(2)}</span>
            <span></span>
        </div>`;
    });

    const spreadInfo = document.getElementById('spreadInfo');
    if (bestBid && bestAsk) {
        spreadInfo.innerText = `Spread: $${((bestAsk - bestBid)/100).toFixed(2)}`;
    } else {
        spreadInfo.innerText = `Spread: —`;
    }
}

function renderMemory() {
    const snap = engine.getMemorySnapshot();
    
    document.getElementById('activeCount').innerText = snap.activeCount;
    document.getElementById('freeCount').innerText = (snap.capacity - snap.activeCount);
    document.getElementById('nextFreeIdx').innerText = snap.nextFree;
    
    const capacity = snap.capacity;
    const pct = (snap.activeCount / capacity) * 100;
    document.getElementById('usedPct').innerText = pct.toFixed(2) + '%';
    document.getElementById('memBarFill').style.width = pct + '%';
    
    // Free list visualization
    const flViz = document.getElementById('freelistViz');
    flViz.innerHTML = '';
    let curr = snap.nextFreeIdx;
    for (let i = 0; i < 8; i++) {
        if (curr === null || curr === undefined) break;
        const cl = i === 0 ? 'freelist-box head' : 'freelist-box';
        flViz.innerHTML += `<div class="${cl}">${curr}</div>`;
        curr = engine.pool.arena[curr].next; // peek into next
    }
    flViz.innerHTML += `<div class="freelist-box" style="border:none;background:transparent;">...</div>`;
    
    // Physical slots grid (first 40 slots)
    const grid = document.getElementById('slotsGrid');
    grid.innerHTML = '';
    for (let i = 0; i < 40; i++) {
        let isUsed = false;
        // In this JS version, slot contains orderId if used, otherwise null/undefined (for free list pointer)
        if (engine.pool.arena[i].orderId !== undefined && engine.pool.arena[i].orderId !== null) {
            isUsed = true;
        }
        const cl = isUsed ? 'slot-box used' : 'slot-box';
        grid.innerHTML += `<div class="${cl}">[${i}]</div>`;
    }
}

function renderNodes() {
    const nodes = engine.getNodeSnapshot(10);
    const tbody = document.getElementById('nodeTableBody');
    const empty = document.getElementById('nodeEmpty');
    const chainViz = document.getElementById('chainViz');
    
    tbody.innerHTML = '';
    chainViz.innerHTML = '';
    
    if (nodes.length === 0) {
        empty.style.display = 'block';
        return;
    }
    
    empty.style.display = 'none';
    
    // Group by price for chain visualization
    const chainsByPrice = {};
    
    nodes.forEach(n => {
        const sideCl = n.side === 'B' ? 'buy-qty' : 'sell-qty';
        
        tbody.innerHTML += `
        <tr>
            <td><strong>${n.slot}</strong></td>
            <td>#${n.id}</td>
            <td class="${sideCl}"><strong>${n.side}</strong></td>
            <td>$${(n.price/100).toFixed(2)}</td>
            <td>${n.qty}</td>
            <td style="color:var(--text-dim); font-weight: 500;">${n.prev === -1 ? 'NONE' : n.prev}</td>
            <td style="color:var(--text-dim); font-weight: 500;">${n.next === -1 ? 'NONE' : n.next}</td>
        </tr>`;
        
        if (!chainsByPrice[n.price]) chainsByPrice[n.price] = [];
        chainsByPrice[n.price].push(n);
    });
    
    // Render chains
    for (const [priceStr, chainNodes] of Object.entries(chainsByPrice)) {
        const price = parseInt(priceStr, 10);
        const side = chainNodes[0].side;
        const sideCl = side === 'B' ? 'b' : 's';
        
        let html = `<div class="chain-level">`;
        html += `<div class="chain-price ${sideCl}">$${(price/100).toFixed(2)}</div>`;
        
        // In the real engine, we'd start at the head pointer. 
        // Here we just display the nodes we grabbed that happen to be at this price.
        chainNodes.forEach((n, idx) => {
            html += `<div class="chain-node">
                <span style="color:var(--text-light)">Slot [${n.slot}]</span>
                <strong>#${n.id} | ${n.qty}</strong>
            </div>`;
            if (idx < chainNodes.length - 1) {
                html += `<div class="chain-arrow">→</div>`;
            }
        });
        
        html += `</div>`;
        chainViz.innerHTML += html;
    }
}

// === Init ===
window.onload = () => {
    engine = new Engine(uiCallback);
    updateAllVisualizers();
    populateHeroTicker();
    
    // Default view
    switchTab('demo');
};
