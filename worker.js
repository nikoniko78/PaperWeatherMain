const REPO = "nikoniko78/PaperWeatherMain";
const ORIGIN = "https://nikoniko78.github.io";

function reply(body, status) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": ORIGIN,
      "Access-Control-Allow-Headers": "Content-Type",
      "Access-Control-Allow-Methods": "POST, OPTIONS"
    }
  });
}

function clean(value, max) {
  return String(value || "").replace(/\r/g, "").trim().slice(0, max);
}

export default {
  async fetch(request, env) {
    if (request.method === "OPTIONS") return reply({}, 204);
    if (request.method !== "POST") return reply({ error: "Not allowed." }, 405);
    if (request.headers.get("Origin") !== ORIGIN) {
      return reply({ error: "Not allowed." }, 403);
    }
    let data;
    try {
      data = await request.json();
    } catch (e) {
      return reply({ error: "Invalid request." }, 400);
    }
    if (data._gotcha) return reply({ number: 0 }, 200);
    const summary = clean(data.summary, 120);
    const details = clean(data.details, 3000);
    if (!summary || !details) {
      return reply({ error: "Please fill in the summary and details." }, 400);
    }
    const area = clean(data.area, 40);
    const why = clean(data.why, 1500);
    const version = clean(data.version, 30);
    const body =
      "**Area:** " + area + "\n" +
      "**Firmware version:** " + (version || "not given") + "\n\n" +
      "### What should it do?\n" + details + "\n\n" +
      "### Why would it help?\n" + (why || "_Not provided_") + "\n\n" +
      "_Posted from the feature request site._";
    const response = await fetch("https://api.github.com/repos/" + REPO + "/issues", {
      method: "POST",
      headers: {
        "Authorization": "Bearer " + env.GITHUB_TOKEN,
        "Accept": "application/vnd.github+json",
        "User-Agent": "yawos-feature-requests",
        "Content-Type": "application/json"
      },
      body: JSON.stringify({
        title: "[Feature] " + summary,
        body: body,
        labels: ["enhancement"]
      })
    });
    if (!response.ok) return reply({ error: "Could not post the request." }, 502);
    const issue = await response.json();
    return reply({ number: issue.number }, 200);
  }
};
