"""Drive the Foreman dashboard through a scripted scenario and record it (frames + screenshots).

Frames come from Chrome's screencast (JPEG, only when the page changes), which costs far less CPU than
encoding video live; ci/encode-demo.sh turns them into an MP4 after the QNX workload has finished, so
the recording does not compete with the virtual machine for host CPU.

Runs on the CI host against the QNX guest (dashboard forwarded to localhost:8080, SSH on 2222).
Usage: python3 ci/record-demo.py <profile> <out dir>
"""
import asyncio
import base64
import os
import subprocess
import sys
import time

from playwright.async_api import async_playwright

PROFILE = sys.argv[1] if len(sys.argv) > 1 else "industrial"
OUT = sys.argv[2] if len(sys.argv) > 2 else "demo"
F = "/data/home/qnxuser/foreman"
SSH = ["sshpass", "-p", "root", "ssh", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
       "-o", "LogLevel=ERROR", "-p", "2222", "root@127.0.0.1"]

# (action, argument): ask = type a question; inject/clear = operator console on the target; wait = seconds
SCRIPTS = {
    "industrial": [
        ("wait", 6),
        ("ask", "Is the line running normally right now?"),
        ("inject", "jam"), ("wait", 7), ("shot", "jam-alarm"),
        ("ask", "The line just stopped. What happened and how do I recover safely?"),
        ("evidence", None),
        ("ask", "Restart the line now."),
        ("clear", None), ("inject", "cooling_fail"), ("wait", 40), ("shot", "derated"),
        ("ask", "Why is the line running slower than normal?"),
        ("ask", "Is the control loop meeting its deadlines while you answer me?"),
        ("wait", 4), ("shot", "end"),
    ],
    "automotive": [
        ("wait", 5), ("inject", "coolant_pump_fail"), ("wait", 40), ("shot", "limp-mode"),
        ("ask", "Why has the car gone into limp mode, and is it safe to keep driving?"),
        ("wait", 3), ("shot", "end"),
    ],
    "medical": [
        ("wait", 5), ("inject", "occlusion"), ("wait", 20), ("shot", "occlusion"),
        ("ask", "The pump stopped. What does alarm A3 mean and what should the nurse check?"),
        ("wait", 3), ("shot", "end"),
    ],
}


def target(cmd):
    try:
        subprocess.run(SSH + [f"cd {F} && ./fmctl {cmd}"], check=False, capture_output=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as e:  # recording continues; the scenario just won't change
        print(f"operator command failed ({cmd}): {e}", flush=True)


async def ask(page, question):
    before = await page.locator(".msg.bot").count()
    await page.click("#q")
    await page.keyboard.type(question, delay=35)  # visible typing for the recording
    await page.click("#send")
    t0 = time.time()
    while await page.locator(".msg.bot").count() == before and time.time() - t0 < 240:
        await page.wait_for_timeout(500)
    await page.wait_for_timeout(3500)  # time to read the answer
    print(f"asked in {time.time() - t0:.0f}s: {question}", flush=True)


async def main():
    async with async_playwright() as p:
        browser = await p.chromium.launch(**({"executable_path": "/usr/bin/google-chrome", "args": ["--no-sandbox"]} if __import__("os").environ.get("LOCAL_CHROME") else {}))
        ctx = await browser.new_context(viewport={"width": 1600, "height": 900}, device_scale_factor=1)
        page = await ctx.new_page()
        os.makedirs(f"{OUT}/frames", exist_ok=True)
        frames = []  # (wall-clock time, file)
        cdp = await ctx.new_cdp_session(page)

        async def on_frame(ev):
            name = f"{OUT}/frames/{len(frames):06d}.jpg"
            with open(name, "wb") as f:
                f.write(base64.b64decode(ev["data"]))
            frames.append((time.time(), name))
            await cdp.send("Page.screencastFrameAck", {"sessionId": ev["sessionId"]})

        cdp.on("Page.screencastFrame", lambda ev: asyncio.ensure_future(on_frame(ev)))
        await page.goto("http://localhost:8080/")
        await cdp.send("Page.startScreencast", {"format": "jpeg", "quality": 85, "maxWidth": 1600, "maxHeight": 900})
        for action, arg in SCRIPTS[PROFILE]:
            if action == "wait":
                await page.wait_for_timeout(arg * 1000)
            elif action == "ask":
                await ask(page, arg)
            elif action == "inject":
                target(f"inject {arg}")
            elif action == "clear":
                target("clear")
            elif action == "shot":
                await page.screenshot(path=f"{OUT}/{PROFILE}-{arg}.png")
            elif action == "evidence":
                last = page.locator(".msg.bot details").last
                if await last.count():
                    await last.locator("summary").click()
                    await page.wait_for_timeout(5000)
                    await page.screenshot(path=f"{OUT}/{PROFILE}-evidence.png")
                    await last.locator("summary").click()
        await cdp.send("Page.stopScreencast")
        frames.append((time.time(), frames[-1][1] if frames else ""))
        with open(f"{OUT}/frames.ffconcat", "w") as f:  # each frame shown until the next one arrived
            f.write("ffconcat version 1.0\n")
            for (t, name), (t2, _) in zip(frames, frames[1:]):
                f.write(f"file '{os.path.basename(name)}'\nduration {max(t2 - t, 0.001):.3f}\n")
        print(f"{len(frames) - 1} frames over {frames[-1][0] - frames[0][0]:.0f} s", flush=True)
        await ctx.close()
        await browser.close()


asyncio.run(main())
