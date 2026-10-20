# Mbake Kekia Portfolio

A personal portfolio website built with three languages working together:

| Layer | Language | File |
|---|---|---|
| Page structure and content | HTML | `public/index.html` |
| Styling, light and dark themes | CSS | `public/style.css` |
| Interactivity, form handling, API calls | JavaScript | `public/script.js` |
| Web server and backend API | C | `server.c` |

## Project layout

```
portfolio/
├── server.c          C web server (static files + API)
├── Makefile          build shortcuts
├── README.md
├── public/           everything the browser downloads
│   ├── index.html
│   ├── style.css
│   └── script.js
└── data/             created automatically on first run
    ├── visits.txt    visit counter
    └── messages.txt  contact form submissions
```

## Run it

You need a C compiler (gcc or clang). On Windows, use WSL (Ubuntu).

```bash
cd portfolio
make            # or: gcc -Wall -Wextra -O2 -o server server.c
./server        # serves on http://localhost:8080
./server 3000   # or pick a port
```

Open http://localhost:8080 in your browser. Press Ctrl+C to stop the server.

Contact form messages are saved to `data/messages.txt`.

## What the C server does

The server opens a TCP socket, accepts connections, and forks a child process for each one so a slow client cannot block anyone else. For every request it reads the headers, parses the method and path, and routes it:

`GET /anything` serves a file from `public/` with the right MIME type.
`GET /api/visits` returns the visit count as JSON.
`POST /api/visits` increments the count, then returns it.
`POST /api/contact` validates the form fields and appends them to `data/messages.txt`.

## Security features (good talking points for interviews)

Path traversal protection: requests containing `..` are rejected, and every file path is resolved with `realpath()` and checked to be inside `public/`, so `/../server.c` and `/%2e%2e/server.c` both return 403.

Request limits: headers capped at 8 KB, bodies at 8 KB (413 if larger), and 5 second socket timeouts against slow clients.

Input sanitizing: control characters and line breaks are stripped from form input so a visitor cannot forge fake entries in the message log. Null bytes in URL encoding are rejected.

File locking: `flock()` keeps two simultaneous requests from corrupting the visit counter or message file.

Security headers: Content-Security-Policy, X-Content-Type-Options, X-Frame-Options, and Referrer-Policy are sent on every response.

## Customizing

Edit the text in `public/index.html` to change your bio, projects, and experience. Project cards use `data-category` (web, security, networking) to work with the filter buttons. Skill bars take their percentage from `data-level`. Colors live in the `:root` block at the top of `style.css`.

## Deploying

The JavaScript only talks to the C server, so opening `index.html` directly from your file system will show the page but the visit counter will read "offline" and the form will not send. To host it publicly, run the compiled server on a Linux VPS (or in a Docker container) behind a reverse proxy such as Nginx or Caddy for HTTPS.
