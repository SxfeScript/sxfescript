#!/usr/bin/env python3
"""Render the repo's markdown into the documentation site under docs/.

Every page on sxfescript.github.io/docs/ is one of the markdown files that
already lives in this repo -- the specs, the README, the contributing guide,
and the two hand-written guide pages in docs/guide/. Keeping the site
generated from those files rather than written twice is the whole point: a
spec edit is a docs edit.

Markdown is rendered by GitHub's own /markdown endpoint through `gh`, so the
tables and fenced code come out exactly as they do on the repo page, and
there is no markdown library to vendor or keep current. `mode=markdown`, not
`mode=gfm`: gfm is the mode GitHub uses for comment boxes, where a single
newline becomes a <br>, which turns every hard-wrapped paragraph in these
files into a column of ragged short lines. The token classes GitHub emits for
syntax highlighting are styled in docs/_page.html.

Usage: scripts/build-docs.py <output-dir> [repo-url]
"""

import html
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

SITE = "https://sxfescript.github.io"

# (source file, url slug, title, sidebar section, one-line description)
PAGES = [
    ("docs/guide/quickstart.md", "quickstart", "Quick start", "Get started",
     "Install sxn, run your first .js, .ts and .sx file, and start an HTTP server."),
    ("docs/guide/examples.md", "examples", "Examples", "Get started",
     "Complete programs you can run, each with the output it actually prints."),
    ("spec/CLI.md", "cli", "CLI reference", "Get started",
     "Every sxn command and flag, and how a file with no extension is resolved."),

    ("spec/LANGUAGE.md", "language", "Language contract", "The language",
     "What .sx adds to JavaScript: erasable types, safe/unsafe, ownership, layout."),
    ("spec/ABI.md", "abi", "ABI", "The language",
     "The boundary between SxfeScript values and native memory."),
    ("spec/BYTECODE.md", "bytecode", "Bytecode", "The language",
     "Compiling to .sxbc, the compile cache, the measured gains, and the trust boundary."),

    ("spec/RUNTIME.md", "runtime", "Runtime surface", "The runtime",
     "The WinterCG web APIs and the Sxn host namespace: fetch, Sxn.serve, streams, crypto, FFI."),
    ("spec/NODE.md", "node", "Node compatibility", "The runtime",
     "CommonJS, the node: builtins, and .node native addons through a from-scratch Node-API."),
    ("spec/NATIVE.md", "native", "Native code", "The runtime",
     "Sxn.ffi and .node addons, and why only one of the two belongs to the engine."),
    ("spec/NETWORK.md", "network", "Networking", "The runtime",
     "The networking primitives the runtime is built on."),

    ("spec/PERFORMANCE.md", "performance", "Performance", "Project",
     "The full benchmark write-up: methodology, both machines, and every optimization behind the numbers."),
    ("spec/BENCHMARK_REFERENCES.md", "benchmark-references", "Benchmark references", "Project",
     "Where the comparison numbers come from."),
    ("spec/IMPLEMENTATION.md", "implementation", "Implementation ledger", "Project",
     "What is real today and what is not yet, kept honest and up to date."),
    ("CONTRIBUTING.md", "contributing", "Contributing", "Project",
     "What is genuinely open to debate, and the one constraint that is not."),
    ("README.md", "readme", "Project README", "Project",
     "The overview, the install commands, and the two benchmark tables."),
]

BY_SOURCE = {source: (slug, title) for source, slug, title, _, _ in PAGES}


INCLUDE_RE = re.compile(r"^<!-- include: (\S+) as (\S+) -->$", re.M)


def expand_includes(text):
    """`<!-- include: examples/server.mjs as js -->` becomes that file inside a
    fenced block. The examples page quotes six programs in full, and a copy
    pasted into the markdown goes stale the moment the program is edited --
    silently, since nothing checks the two against each other."""

    def one(m):
        source = (ROOT / m.group(1)).read_text().rstrip()
        return f"```{m.group(2)}\n{source}\n```"

    return INCLUDE_RE.sub(one, text)


# GitHub has no highlighter for .sx, and an ```sx fence comes back as flat
# unhighlighted text. TypeScript's covers the language almost exactly -- it is
# JavaScript plus the same annotation syntax -- so the fence is relabelled on
# the way in and the three keywords TypeScript does not know are re-tagged as
# keywords on the way out. The markdown files stay honest and say `sx`.
SX_FENCE_RE = re.compile(r"^```sx$", re.M)
SX_KEYWORDS = ("mut", "safe", "unsafe")


def retag_sx_keywords(body):
    for word in SX_KEYWORDS:
        body = body.replace(f'<span class="pl-s1">{word}</span>',
                            f'<span class="pl-k">{word}</span>')
    return body


def render_markdown(text):
    """GitHub's own renderer, through gh so the call is authenticated."""
    text = SX_FENCE_RE.sub("```ts", text)
    try:
        out = subprocess.run(
            ["gh", "api", "-X", "POST", "/markdown", "-f", "mode=markdown", "-f", "text=" + text],
            check=True, capture_output=True, text=True,
        )
    except FileNotFoundError:
        sys.exit("build-docs: gh is not installed; it renders the markdown")
    except subprocess.CalledProcessError as e:
        sys.exit("build-docs: gh api /markdown failed: " + e.stderr.strip())
    return out.stdout


def slugify(text):
    text = re.sub(r"<[^>]+>", "", text)
    text = html.unescape(text).lower()
    text = re.sub(r"[^\w\s-]", "", text)
    return re.sub(r"[\s_]+", "-", text.strip())[:60] or "section"


def add_heading_anchors(body):
    """GitHub's API returns headings with no ids, so deep links need these."""
    seen = {}

    def one(m):
        level, attrs, text = m.group(1), m.group(2), m.group(3)
        slug = slugify(text)
        seen[slug] = seen.get(slug, 0) + 1
        if seen[slug] > 1:
            slug = f"{slug}-{seen[slug] - 1}"
        anchor = f'<a class="anchor" href="#{slug}" aria-hidden="true">#</a>'
        return f'<h{level} id="{slug}"{attrs}>{anchor}{text}</h{level}>'

    return re.sub(r"<h([23])([^>]*)>(.*?)</h\1>", one, body, flags=re.S)


def wrap_tables(body):
    """A wide table has to scroll inside its own box, or it pushes the whole
    page sideways on a narrow screen."""
    return (body.replace("<table>", '<div class="table-scroll"><table>')
                .replace("</table>", "</table></div>"))


def rewrite_links(body, root, repo_url):
    """Point cross-references at this site where the target is a page here,
    and at the repo where it is a file that has no page."""

    def one(m):
        href = html.unescape(m.group(1))
        if href.startswith(("http://", "https://", "#", "mailto:")):
            return m.group(0)
        target, _, fragment = href.partition("#")
        target = target.lstrip("./")
        if target in BY_SOURCE:
            slug = BY_SOURCE[target][0]
            new = f"{root}docs/{slug}/"
        elif target in ("docs/index.html", "docs/", ""):
            new = root or "/"
        elif target:
            new = f"{repo_url}/blob/main/{target}"
        else:
            return m.group(0)
        if fragment:
            new += "#" + fragment
        return f'href="{html.escape(new)}"'

    return re.sub(r'href="([^"]*)"', one, body)


SOURCE_RE = re.compile(r"<!-- source: (\S+) -->")
# An inline snippet that is an illustration rather than a runnable file -- the
# hero's TypeScript-versus-SxfeScript pair. Same renderer, so the hero and the
# example panels below it are highlighted by one thing rather than two.
INLINE_RE = re.compile(r"<!-- highlight: (\S+) -->(.*?)<!-- /highlight -->", re.S)


def render_index(repo_url):
    """The landing page's example panels name a file rather than carrying a
    copy of it, for the same reason the examples page does: a pasted copy goes
    stale the moment the program is edited, silently. Each marker becomes that
    file, highlighted by the same renderer the doc pages use."""
    page = (ROOT / "docs" / "index.html").read_text()

    def one(m):
        source = (ROOT / m.group(1)).read_text().rstrip()
        # Drop the file's own header comment. It says how to run the program,
        # which the panel already shows on the line above and the line below.
        lines = source.splitlines()
        while lines and (lines[0].startswith("//") or not lines[0].strip()):
            lines.pop(0)
        source = "\n".join(lines)
        return retag_sx_keywords(render_markdown(f"```sx\n{source}\n```")).strip()

    def inline(m):
        snippet = html.unescape(m.group(2)).strip("\n")
        rendered = render_markdown(f"```{m.group(1)}\n{snippet}\n```")
        return retag_sx_keywords(rendered).strip()

    page = SOURCE_RE.sub(one, page)
    page = INLINE_RE.sub(inline, page)
    return page.replace("{{REPO_URL}}", repo_url)


def sidebar_html(current_slug, root):
    out = []
    section = None
    for _, slug, title, group, _ in PAGES:
        if group != section:
            if section is not None:
                out.append("      </ul>")
            out.append(f"      <h4>{html.escape(group)}</h4>")
            out.append("      <ul>")
            section = group
        cls = ' class="current"' if slug == current_slug else ""
        out.append(f'        <li><a{cls} href="{root}docs/{slug}/">{html.escape(title)}</a></li>')
    out.append("      </ul>")
    return "\n".join(out)


def prevnext_html(index, root):
    out = []
    if index > 0:
        _, slug, title, _, _ = PAGES[index - 1]
        out.append(f'        <a class="prev" href="{root}docs/{slug}/">'
                   f'<span>Previous</span>{html.escape(title)}</a>')
    if index < len(PAGES) - 1:
        _, slug, title, _, _ = PAGES[index + 1]
        out.append(f'        <a class="next" href="{root}docs/{slug}/">'
                   f'<span>Next</span>{html.escape(title)}</a>')
    return "\n".join(out)


def build(out_dir, repo_url):
    out_dir = pathlib.Path(out_dir)
    template = (ROOT / "docs" / "_page.html").read_text()
    rendered = []

    for index, (source, slug, title, _, description) in enumerate(PAGES):
        text = expand_includes((ROOT / source).read_text())
        # Depth from /docs/<slug>/ back to the site root.
        root = "../../"
        body = retag_sx_keywords(render_markdown(text))
        body = add_heading_anchors(body)
        body = wrap_tables(body)
        body = rewrite_links(body, root, repo_url)
        page = (template
                .replace("{{TITLE}}", html.escape(title))
                .replace("{{DESCRIPTION}}", html.escape(description))
                .replace("{{SIDEBAR}}", sidebar_html(slug, root))
                .replace("{{PREVNEXT}}", prevnext_html(index, root))
                .replace("{{CONTENT}}", body)
                .replace("{{SOURCE}}", source)
                .replace("{{ROOT}}", root)
                .replace("{{REPO_URL}}", repo_url))
        target = out_dir / "docs" / slug
        target.mkdir(parents=True, exist_ok=True)
        (target / "index.html").write_text(page)
        rendered.append((source, slug, title, description, text))
        print(f"  docs/{slug}/", file=sys.stderr)

    (out_dir / "index.html").write_text(render_index(repo_url))
    print("  index.html", file=sys.stderr)

    # /docs/ itself is the quick start, so the nav link lands somewhere real.
    first = (out_dir / "docs" / PAGES[0][1] / "index.html").read_text()
    (out_dir / "docs" / "index.html").write_text(first.replace('"../../', '"../'))

    write_llms(out_dir, rendered)


def write_llms(out_dir, rendered):
    """llms.txt is the map, llms-full.txt is the territory: the first is an
    index a model can fetch cheaply, the second is every page's markdown in
    one file so nothing has to be fetched at all."""
    lines = [
        "# SxfeScript and SXN",
        "",
        "> SxfeScript is JavaScript with an explicit safe/unsafe boundary, affine "
        "values and lexical borrows, plus erasable TypeScript annotations, all parsed "
        "natively with no build step. It runs on ArcSX, a QuickJS-based runtime built "
        "as `sxn` that is designed to run the same on a phone as it does on a server.",
        "",
        "ArcSX implements the WinterCG web APIs (fetch, Sxn.serve, Web Streams, Web "
        "Crypto) and a Node compatibility layer (CommonJS, node: builtins, .node "
        "native addons). It has no JIT, deliberately: iOS will not grant a "
        "third-party app the entitlement to generate machine code, and running there "
        "is the point.",
        "",
        f"- Source: {SITE.replace('sxfescript.github.io', 'github.com/SxfeScript/sxfescript')}",
        f"- Every page below, in full: {SITE}/llms-full.txt",
        "",
    ]
    section = None
    for source, slug, title, description, _ in rendered:
        group = next(g for s, _, _, g, _ in PAGES if s == source)
        if group != section:
            if section is not None:
                lines.append("")
            lines.append(f"## {group}")
            lines.append("")
            section = group
        lines.append(f"- [{title}]({SITE}/docs/{slug}/): {description}")
    lines.append("")
    (out_dir / "llms.txt").write_text("\n".join(lines))

    full = [
        "# SxfeScript and SXN, complete documentation",
        "",
        f"Every documentation page from {SITE}/docs/, in full, in source order.",
        f"The index with per-page links is at {SITE}/llms.txt.",
        "",
    ]
    for source, slug, title, _, text in rendered:
        full.append("=" * 78)
        full.append(f"# {title}")
        full.append(f"Source: {source}")
        full.append(f"URL: {SITE}/docs/{slug}/")
        full.append("=" * 78)
        full.append("")
        full.append(text.strip())
        full.append("")
    (out_dir / "llms-full.txt").write_text("\n".join(full))
    print("  llms.txt, llms-full.txt", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2
          else "https://github.com/SxfeScript/sxfescript")
