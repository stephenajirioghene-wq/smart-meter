// Chart.js wrappers (Chart is the global from the UMD <script> tag).
// Colours come from CSS custom properties so light and dark mode both work.

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

function baseOptions(unit) {
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    interaction: { mode: "index", intersect: false },
    plugins: {
      legend: { display: false },
      tooltip: { callbacks: { label: (c) => `${c.parsed.y} ${unit}` } }
    },
    scales: {
      x: {
        grid: { display: false },
        border: { color: css("--axis") },
        ticks: { color: css("--muted"), maxRotation: 0, autoSkip: true, maxTicksLimit: 8 }
      },
      y: {
        beginAtZero: true,
        grid: { color: css("--grid") },
        border: { display: false },
        ticks: { color: css("--muted") },
        title: { display: true, text: unit, color: css("--muted") }
      }
    }
  };
}

// 24 h power line chart (W)
export function powerLineChart(canvas) {
  return new Chart(canvas, {
    type: "line",
    data: { labels: [], datasets: [{
      data: [], borderColor: css("--series-1"), borderWidth: 2,
      pointRadius: 0, pointHoverRadius: 4, tension: 0.2, spanGaps: false
    }] },
    options: baseOptions("W")
  });
}

// 7-day energy bar chart (kWh or units)
export function dailyBarChart(canvas, unit) {
  return new Chart(canvas, {
    type: "bar",
    data: { labels: [], datasets: [{
      data: [], backgroundColor: css("--series-1"),
      borderRadius: { topLeft: 4, topRight: 4 }, borderSkipped: "bottom",
      maxBarThickness: 36
    }] },
    options: baseOptions(unit)
  });
}

export function setChartData(chart, labels, values) {
  chart.data.labels = labels;
  chart.data.datasets[0].data = values;
  chart.update("none");
}
