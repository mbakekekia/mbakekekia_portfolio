/* ============================================================
   Mbake Kekia Portfolio  |  script.js
   Talks to the C server at /api/visits and /api/contact
   ============================================================ */

(function () {
  "use strict";

  const $ = (sel, ctx = document) => ctx.querySelector(sel);
  const $$ = (sel, ctx = document) => Array.from(ctx.querySelectorAll(sel));

  /* ---------- Footer year ---------- */
  $("#year").textContent = new Date().getFullYear();

  /* ---------- Theme toggle (remembered per browser) ---------- */
  const root = document.documentElement;
  const themeBtn = $("#themeToggle");
  const themeIcon = $(".theme-icon");

  function applyTheme(theme) {
    root.setAttribute("data-theme", theme);
    themeIcon.innerHTML = theme === "dark" ? "&#9790;" : "&#9728;";
  }

  let savedTheme = null;
  try { savedTheme = localStorage.getItem("theme"); } catch (e) { /* storage blocked */ }
  if (!savedTheme) {
    savedTheme = window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
  }
  applyTheme(savedTheme);

  themeBtn.addEventListener("click", () => {
    const next = root.getAttribute("data-theme") === "dark" ? "light" : "dark";
    applyTheme(next);
    try { localStorage.setItem("theme", next); } catch (e) { /* ignore */ }
  });

  /* ---------- Mobile menu ---------- */
  const navToggle = $("#navToggle");
  const navLinks = $("#navLinks");

  navToggle.addEventListener("click", () => {
    const open = navLinks.classList.toggle("open");
    navToggle.classList.toggle("open", open);
    navToggle.setAttribute("aria-expanded", String(open));
  });

  $$(".nav-link").forEach((link) =>
    link.addEventListener("click", () => {
      navLinks.classList.remove("open");
      navToggle.classList.remove("open");
      navToggle.setAttribute("aria-expanded", "false");
    })
  );

  /* ---------- Typing effect in the hero ---------- */
  const roles = [
    "web developer.",
    "future security analyst.",
    "networking nerd.",
    "content creator.",
    "C programmer in training."
  ];
  const typedEl = $("#typed");
  let roleIndex = 0;
  let charIndex = 0;
  let deleting = false;

  function typeLoop() {
    const word = roles[roleIndex];
    typedEl.textContent = word.slice(0, charIndex);

    if (!deleting && charIndex < word.length) {
      charIndex++;
      setTimeout(typeLoop, 80);
    } else if (!deleting && charIndex === word.length) {
      deleting = true;
      setTimeout(typeLoop, 1600);
    } else if (deleting && charIndex > 0) {
      charIndex--;
      setTimeout(typeLoop, 40);
    } else {
      deleting = false;
      roleIndex = (roleIndex + 1) % roles.length;
      setTimeout(typeLoop, 300);
    }
  }

  if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
    typedEl.textContent = roles[0];
  } else {
    typeLoop();
  }

  /* ---------- Scroll reveal + skill bar animation ---------- */
  const revealObserver = new IntersectionObserver(
    (entries) => {
      entries.forEach((entry) => {
        if (!entry.isIntersecting) return;
        entry.target.classList.add("visible");

        $$(".skill-bar", entry.target).forEach((bar) => {
          $(".fill", bar).style.width = bar.dataset.level + "%";
        });

        revealObserver.unobserve(entry.target);
      });
    },
    { threshold: 0.15 }
  );
  $$(".reveal").forEach((el) => revealObserver.observe(el));

  /* ---------- Header shadow, active link, back to top ---------- */
  const header = $(".site-header");
  const toTop = $("#toTop");
  const sections = $$("main section[id]");
  const links = $$(".nav-link");

  function onScroll() {
    const y = window.scrollY;
    header.classList.toggle("scrolled", y > 10);
    toTop.classList.toggle("show", y > 600);

    let current = "";
    sections.forEach((sec) => {
      if (y >= sec.offsetTop - 120) current = sec.id;
    });
    links.forEach((l) =>
      l.classList.toggle("active", l.getAttribute("href") === "#" + current)
    );
  }
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  toTop.addEventListener("click", () => window.scrollTo({ top: 0 }));

  /* ---------- Project filters ---------- */
  const filterBtns = $$(".filter");
  const cards = $$(".project-card");

  filterBtns.forEach((btn) =>
    btn.addEventListener("click", () => {
      filterBtns.forEach((b) => b.classList.remove("active"));
      btn.classList.add("active");
      const f = btn.dataset.filter;
      cards.forEach((card) => {
        const cats = card.dataset.category.split(" ");
        card.classList.toggle("hidden", f !== "all" && !cats.includes(f));
      });
    })
  );

  /* ---------- Visit counter (served by the C backend) ---------- */
  fetch("/api/visits", { method: "POST" })
    .then((r) => (r.ok ? r.json() : Promise.reject()))
    .then((data) => { $("#visitCount").textContent = data.visits; })
    .catch(() => { $("#visitCount").textContent = "offline"; });

  /* ---------- Contact form (validated here, stored by the C backend) ---------- */
  const form = $("#contactForm");
  const statusEl = $("#formStatus");
  const submitBtn = $("#submitBtn");
  const emailPattern = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

  function setStatus(msg, type) {
    statusEl.textContent = msg;
    statusEl.className = "form-status " + (type || "");
  }

  form.addEventListener("submit", async (e) => {
    e.preventDefault();

    const fields = {
      name: form.name.value.trim(),
      email: form.email.value.trim(),
      message: form.message.value.trim()
    };

    let valid = true;
    $$(".field", form).forEach((f) => f.classList.remove("error"));

    if (!fields.name) { form.name.parentElement.classList.add("error"); valid = false; }
    if (!emailPattern.test(fields.email)) { form.email.parentElement.classList.add("error"); valid = false; }
    if (fields.message.length < 10) { form.message.parentElement.classList.add("error"); valid = false; }

    if (!valid) {
      setStatus("Please add your name, a valid email, and a message of at least 10 characters.", "err");
      return;
    }

    submitBtn.disabled = true;
    setStatus("Sending...");

    try {
      const res = await fetch("/api/contact", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: new URLSearchParams(fields).toString()
      });
      const data = await res.json();
      if (!res.ok || !data.ok) throw new Error(data.error || "Server error");
      setStatus("Thanks! Your message was received. I'll get back to you soon.", "ok");
      form.reset();
    } catch (err) {
      setStatus("Something went wrong. Please email me directly at mke287@uky.edu.", "err");
    } finally {
      submitBtn.disabled = false;
    }
  });
})();
