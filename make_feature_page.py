import os
import sys

WORKER_URL = sys.argv[1] if len(sys.argv) > 1 else "__WORKER_URL__"

worker_code = """
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
"""

with open("worker.js", "w", encoding="utf-8") as f:
    f.write(worker_code.strip())

os.makedirs("docs", exist_ok=True)
with open("docs/.nojekyll", "w", encoding="utf-8") as f:
    f.write("")

html_code = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Feature Request</title>
  <style>
    body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; max-width: 600px; margin: 40px auto; padding: 0 20px; line-height: 1.6; color: #333; }}
    h1 {{ font-size: 1.8rem; margin-bottom: 20px; }}
    label {{ display: block; margin-top: 15px; font-weight: bold; }}
    input[type="text"], textarea {{ width: 100%; padding: 10px; margin-top: 5px; box-sizing: border-box; border: 1px solid #ccc; border-radius: 6px; font-size: 1rem; }}
    textarea {{ height: 120px; resize: vertical; }}
    button {{ margin-top: 20px; padding: 12px 20px; font-size: 1rem; background-color: #0969da; color: white; border: none; border-radius: 6px; cursor: pointer; }}
    button:disabled {{ background-color: #8cbaee; cursor: not-allowed; }}
    #status {{ margin-top: 15px; font-weight: bold; }}
    .success {{ color: #1a7f37; }}
    .error {{ color: #cf222e; }}
  </style>
</head>
<body>
  <h1>Submit a Feature Request</h1>
  <form id="featureForm">
    <label for="title">Feature Title</label>
    <input type="text" id="title" required placeholder="e.g., Add 7-day weather forecast page">

    <label for="body">Description</label>
    <textarea id="body" required placeholder="Explain what you would like to see..."></textarea>

    <button type="submit" id="submitBtn">Send Request</button>
  </form>
  <div id="status"></div>

  <script>
    const ENDPOINT = "{WORKER_URL}";
    const form = document.getElementById("featureForm");
    const statusDiv = document.getElementById("status");
    const submitBtn = document.getElementById("submitBtn");

    form.addEventListener("submit", async (e) => {{
      e.preventDefault();
      
      if (ENDPOINT === "__WORKER_URL__" || !ENDPOINT.startsWith("http")) {{
        statusDiv.className = "error";
        statusDiv.textContent = "This form is not connected yet.";
        return;
      }}

      statusDiv.className = "";
      statusDiv.textContent = "Sending...";
      submitBtn.disabled = true;

      try {{
        const response = await fetch(ENDPOINT, {{
          method: "POST",
          headers: {{ "Content-Type": "application/json" }},
          body: JSON.stringify({{
            title: document.getElementById("title").value,
            body: document.getElementById("body").value
          }})
        }});

        const result = await response.json();

        if (response.ok && result.success) {{
          statusDiv.className = "success";
          statusDiv.innerHTML = 'Submitted successfully! <a href="' + result.issue_url + '" target="_blank">View on GitHub</a>';
          form.reset();
        }} else {{
          statusDiv.className = "error";
          statusDiv.textContent = "Could not send: " + (result.error || "Unknown error");
        }}
      }} catch (err) {{
        statusDiv.className = "error";
        statusDiv.textContent = "Could not send, check your connection.";
      }} finally {{
        submitBtn.disabled = false;
      }}
    }});
  </script>
</body>
</html>
"""

with open("docs/index.html", "w", encoding="utf-8") as f:
    f.write(html_code)

print("Successfully generated worker.js and docs/index.html!")
