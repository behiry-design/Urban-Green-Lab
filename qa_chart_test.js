const { chromium } = require('playwright');
const path = require('path');

const FAKE_SUPABASE_JS = `
window.supabase = {
  createClient(url, key) {
    const now = Date.now();
    const readings = Array.from({ length: 20 }, (_, i) => ({
      recorded_at: new Date(now - (20 - i) * 15 * 60000).toISOString(),
      temp_c: 22 + Math.sin(i / 3) * 4,
      humidity_pct: 60 + Math.cos(i / 3) * 8,
      soil_moisture_pct: 50 - i * 0.3,
    }));
    const kit = { id: 'K01', team_name: 'Team 1', fruit_type: 'strawberry' };
    const healthScore = { kit_id: 'K01', score: 62, status: 'watch', contributing_factors: { vpd_out_of_band: true }, updated_at: new Date().toISOString() };

    function chain(result) {
      const obj = {
        select() { return obj; },
        eq() { return obj; },
        gte() { return obj; },
        order() { return obj; },
        limit() { return obj; },
        maybeSingle() { return Promise.resolve(result); },
        then(resolve) { return Promise.resolve(result).then(resolve); },
      };
      return obj;
    }

    return {
      from(table) {
        if (table === 'kits_public') return chain({ data: kit, error: null });
        if (table === 'health_scores') return chain({ data: healthScore, error: null });
        if (table === 'readings') return chain({ data: readings, error: null });
        if (table === 'photos') return chain({ data: [], error: null });
        return chain({ data: [], error: null });
      },
      rpc() { return Promise.resolve({ data: null, error: null }); },
      storage: { from() { return { getPublicUrl: () => ({ data: { publicUrl: '' } }), upload: () => Promise.resolve({ error: null }) }; } },
    };
  }
};
`;

(async () => {
  const browser = await chromium.launch({ executablePath: '/opt/pw-browsers/chromium' });
  const page = await browser.newPage();

  const consoleErrors = [];
  page.on('console', msg => { if (msg.type() === 'error') consoleErrors.push(msg.text()); });
  page.on('pageerror', err => consoleErrors.push('pageerror: ' + err.message));

  // Intercept the supabase-js CDN request and serve our fake stub instead,
  // so we exercise the REAL chart.js + REAL date adapter from the CDN.
  await page.route('**/supabase-js@2/dist/umd/supabase.js', route => {
    route.fulfill({ status: 200, contentType: 'application/javascript', body: FAKE_SUPABASE_JS });
  });

  await page.addInitScript(() => {
    localStorage.setItem('gh_kit_creds', JSON.stringify({ kitId: 'K01', token: 'faketoken' }));
  });

  const filePath = 'file://' + path.resolve(__dirname, 'dashboard/index.html') + '#/team';
  await page.goto(filePath);
  await page.waitForTimeout(2000);

  const chartInfo = await page.evaluate(() => {
    const tempCanvas = document.getElementById('chart-temp');
    const moistCanvas = document.getElementById('chart-moisture');
    return {
      hasChartTemp: !!(window.Chart && Chart.getChart && Chart.getChart(tempCanvas)),
      hasChartMoisture: !!(window.Chart && Chart.getChart && Chart.getChart(moistCanvas)),
      tempCanvasExists: !!tempCanvas,
      moistCanvasExists: !!moistCanvas,
      bodyText: document.getElementById('app').innerText.slice(0, 300),
    };
  });

  console.log('Console/page errors:', JSON.stringify(consoleErrors, null, 2));
  console.log('Chart info:', JSON.stringify(chartInfo, null, 2));

  await browser.close();
})();
