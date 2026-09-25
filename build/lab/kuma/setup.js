// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The test lab's Uptime Kuma, set up without a browser: the administrator account on
// a fresh Kuma, then the monitors below, through Kuma's own socket API - the one its
// web page uses. Kuma has no environment variable or file for either.
//
// Run inside the Kuma container, where node and socket.io-client already are, by
// build/lab/lab.sh kuma-setup:
//
//   docker exec -e HMLAB_KUMA_ADMIN_PASSWORD -e HMLAB_GITLAB_TOKEN hmlab-kuma-1 node /hmlab/setup.js
//
// The password and the token come from the environment docker exec passes through by
// name, so neither is on a command line. Nothing secret is printed.
//
// It can run again at any time. A monitor that already exists, by name, is left as it
// is - except the two GitLab runner monitors, which are made paused with a
// placeholder header until HMLAB_GITLAB_TOKEN is given, and are then given the token
// and resumed.

"use strict";

const { io } = require("/app/node_modules/socket.io-client");

const ADMIN = "admin";
const password = process.env.HMLAB_KUMA_ADMIN_PASSWORD || "";
const gitlabToken = process.env.HMLAB_GITLAB_TOKEN || "";
const PLACEHOLDER = "put a read_api token in ~/hmlab/gitlab-token and run lab.sh kuma-setup";

if (password.length < 12) {
    console.error("HMLAB_KUMA_ADMIN_PASSWORD is not set (lab.sh reads it from ~/hmlab/secrets.env).");
    process.exit(2);
}

// What Kuma's own editor sends for a new monitor (src/pages/EditMonitor.vue,
// monitorDefaults, Kuma 2.5.5), less the fields only the page uses. Every key here
// is a column; one that is not would fail the insert.
function base(name, description) {
    return {
        type: "http",
        name,
        description,
        parent: null,
        url: "https://",
        wsSubprotocol: "",
        method: "GET",
        protocol: null,
        location: "world",
        ipFamily: null,
        interval: 60,
        retryInterval: 60,
        resendInterval: 0,
        maxretries: 2,
        timeout: 48,
        retryOnlyOnStatusCodeFailure: false,
        notificationIDList: {},
        ignoreTls: false,
        upsideDown: false,
        expiryNotification: false,
        domainExpiryNotification: false,
        maxredirects: 10,
        accepted_statuscodes: ["200-299"],
        saveResponse: false,
        saveErrorResponse: true,
        responseMaxLength: 1024,
        dns_resolve_type: "A",
        dns_resolve_server: "",
        docker_container: "",
        docker_host: null,
        proxyId: null,
        basic_auth_user: "",
        basic_auth_pass: "",
        bearer_token: "",
        mqttUsername: "",
        mqttPassword: "",
        mqttTopic: "",
        mqttWebsocketPath: "",
        mqttSuccessMessage: "",
        mqttCheckType: "keyword",
        authMethod: null,
        oauth_auth_method: "client_secret_basic",
        httpBodyEncoding: "json",
        kafkaProducerBrokers: [],
        kafkaProducerSaslOptions: { mechanism: "None" },
        cacheBust: false,
        kafkaProducerSsl: false,
        kafkaProducerAllowAutoTopicCreation: false,
        gamedigGivenPortOnly: true,
        gamedigToken: "",
        remote_browser: null,
        screenshot_delay: 0,
        rabbitmqNodes: [],
        rabbitmqUsername: "",
        rabbitmqPassword: "",
        conditions: [],
        system_service_name: "",
        sshAuthMethod: "password",
        ntpStratumThreshold: 5,
        ntpTimeOffsetThreshold: 1000,
        ntpRootDispersionThreshold: 500,
    };
}

function http(name, description, url, interval) {
    return Object.assign(base(name, description), { type: "http", url, interval, timeout: Math.floor(interval * 0.8) });
}

function port(name, description, hostname, portNumber) {
    return Object.assign(base(name, description), { type: "port", hostname, port: portNumber });
}

// A GitLab runner's status from GitLab's API: GET /runners/:id answers
// {"status": "online", ...} to a token that can see the runner. Every five minutes,
// well inside the API's rate limit.
function runner(name, id, where) {
    return Object.assign(base(name, `GitLab runner ${id} (${where}): its status from the GitLab API, which must say online. Needs a read_api token of a maintainer of the project in ~/hmlab/gitlab-token.`), {
        type: "json-query",
        url: `https://gitlab.com/api/v4/runners/${id}`,
        interval: 300,
        retryInterval: 300,
        timeout: 30,
        jsonPath: "status",
        jsonPathOperator: "==",
        expectedValue: "online",
        headers: JSON.stringify({ "PRIVATE-TOKEN": gitlabToken || PLACEHOLDER }),
        active: gitlabToken !== "",
    });
}

const MONITORS = [
    http("Forgejo", "The project's own forge on linix (container forgejo, port 3000).", "http://192.168.11.154:3000/", 60),
    runner("GitLab runner linix", 56743206, "linix, the Linux jobs"),
    runner("GitLab runner bench150", 56743216, "bench150, the Windows gate"),
    http("Update feed", "https://updates.progressiverobot.com - the release-chain row's update feed. Expected DOWN until GitLab Pages serves it.", "https://updates.progressiverobot.com/", 300),
    port("bench150 SMTP", "The second Windows bench's hMailServer, SMTP on 192.168.11.150:25.", "192.168.11.150", 25),
    port("Smoke VM SSH", "The installer smoke VM's SSH on 192.168.11.175:22.", "192.168.11.175", 22),
];

function call(socket, event, ...args) {
    return new Promise((resolve) => socket.emit(event, ...args, resolve));
}

async function main() {
    const socket = io("http://127.0.0.1:3001", { transports: ["websocket"], reconnection: false, timeout: 15000 });

    const monitorList = new Promise((resolve) => socket.on("monitorList", resolve));
    // Kuma adds its handlers for a new connection only after an await
    // (server.js, io.on("connection")), and anything sent before then is dropped
    // unanswered - which hung the first version of this script on a fresh Kuma.
    // loginRequired (autoLogin, with authentication off) is emitted once every
    // handler is in place, so nothing is asked before it.
    await new Promise((resolve, reject) => {
        socket.on("loginRequired", resolve);
        socket.on("autoLogin", resolve);
        socket.on("connect_error", reject);
    });

    if (await call(socket, "needSetup")) {
        const made = await call(socket, "setup", ADMIN, password);
        if (!made.ok) {
            throw new Error("setup refused: " + made.msg);
        }
        console.log(`administrator "${ADMIN}" created`);
    } else {
        console.log(`administrator already set up`);
    }

    const login = await call(socket, "login", { username: ADMIN, password, token: "" });
    if (!login.ok) {
        throw new Error("login refused: " + login.msg + " (the password in ~/hmlab/secrets.env is not the one Kuma was set up with)");
    }

    const existing = Object.values(await monitorList);
    for (const wanted of MONITORS) {
        const found = existing.find((m) => m.name === wanted.name);
        if (!found) {
            const added = await call(socket, "add", wanted);
            if (!added.ok) {
                throw new Error(`adding "${wanted.name}" refused: ${added.msg}`);
            }
            console.log(`added   ${wanted.type.padEnd(10)} ${wanted.name}${wanted.active === false ? " (paused: no GitLab token yet)" : ""}`);
            continue;
        }
        if (wanted.type === "json-query" && gitlabToken !== "" && !found.active) {
            // The token has arrived since the monitor was made: give it the header and start it.
            const edited = Object.assign({}, found, { headers: wanted.headers });
            const saved = await call(socket, "editMonitor", edited);
            if (!saved.ok) {
                throw new Error(`updating "${wanted.name}" refused: ${saved.msg}`);
            }
            const resumed = await call(socket, "resumeMonitor", found.id);
            console.log(`updated ${wanted.type.padEnd(10)} ${wanted.name} (token set${resumed && resumed.ok ? ", resumed" : ""})`);
            continue;
        }
        console.log(`kept    ${found.type.padEnd(10)} ${found.name}${found.active ? "" : " (paused)"}`);
    }

    socket.disconnect();
}

// Never left hanging: a Kuma that stops answering fails the setup after a minute.
setTimeout(() => {
    console.error("Kuma did not answer within a minute");
    process.exit(1);
}, 60000).unref();

main().then(() => process.exit(0), (e) => {
    console.error(String(e && e.message ? e.message : e));
    process.exit(1);
});
