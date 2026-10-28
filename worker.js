export default {
  async fetch(request, env, ctx) {
    const ORIGIN = "https://nikoniko78.github.io";

    const corsHeaders = {
      "Access-Control-Allow-Origin": ORIGIN,
      "Access-Control-Allow-Methods": "POST, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type",
    };

    if (request.method === "OPTIONS") {
      return new Response(null, {
        status: 204,
        headers: corsHeaders,
      });
    }

    if (request.method !== "POST") {
      return new Response(JSON.stringify({ error: "Method not allowed" }), {
        status: 405,
        headers: { ...corsHeaders, "Content-Type": "application/json" },
      });
    }

    try {
      const data = await request.json();
      const { title, body } = data;

      if (!title || !body) {
        return new Response(JSON.stringify({ error: "Title and body required" }), {
          status: 400,
          headers: { ...corsHeaders, "Content-Type": "application/json" },
        });
      }

      const res = await fetch("https://api.github.com/repos/nikoniko78/PaperWeatherMain/issues", {
        method: "POST",
        headers: {
          "Authorization": `Bearer ${env.GITHUB_TOKEN}`,
          "User-Agent": "Cloudflare-Worker-Feature-Request",
          "Accept": "application/vnd.github.v3+json",
          "Content-Type": "application/json",
        },
        body: JSON.stringify({
          title: `[Feature Request] ${title}`,
          body: body,
          labels: ["feature-request"],
        }),
      });

      if (!res.ok) {
        const errText = await res.text();
        return new Response(JSON.stringify({ error: "GitHub API error", details: errText }), {
          status: res.status,
          headers: { ...corsHeaders, "Content-Type": "application/json" },
        });
      }

      const issue = await res.json();
      return new Response(JSON.stringify({ success: true, issue_url: issue.html_url }), {
        status: 200,
        headers: { ...corsHeaders, "Content-Type": "application/json" },
      });

    } catch (err) {
      return new Response(JSON.stringify({ error: err.message }), {
        status: 500,
        headers: { ...corsHeaders, "Content-Type": "application/json" },
      });
    }
  }
};