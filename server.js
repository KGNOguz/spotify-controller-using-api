const express = require("express");
const sharp = require("sharp");
const bcrypt = require("bcrypt");
const crypto = require("crypto");
const { createClient } = require("@supabase/supabase-js");
const fetch = (...args) => import("node-fetch").then(({ default: fetch }) => fetch(...args));
require("dotenv").config();

const app = express();
app.use(express.json());

// CORS
app.use((req, res, next) => {
    res.header("Access-Control-Allow-Origin", "*");
    res.header("Access-Control-Allow-Headers", "Content-Type, Authorization, x-device-id, x-device-secret");
    res.header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    if (req.method === "OPTIONS") return res.sendStatus(200);
    next();
});

const supabase = createClient(
    process.env.SUPABASE_URL,
    process.env.SUPABASE_SERVICE_ROLE_KEY
);

// ---------------- JWT USER AUTH ----------------

async function authenticateUser(req, res, next) {
    const token = req.headers.authorization?.split(" ")[1] || req.query.access_token;
    if (!token) return res.sendStatus(401);

    const { data, error } = await supabase.auth.getUser(token);
    if (error || !data.user) return res.sendStatus(403);

    req.user = data.user;
    next();
}

// ---------------- DEVICE AUTH ----------------

async function deviceAuthenticate(req, res, next) {
    const device_id = req.headers["x-device-id"];
    const device_secret = req.headers["x-device-secret"];

    if (!device_id || !device_secret)
        return res.status(401).json({ message: "Missing device credentials" });

    const { data: device } = await supabase
        .from("devices")
        .select("user_id, device_secret")
        .eq("device_id", device_id)
        .single();

    if (!device)
        return res.status(401).json({ message: "Device not registered" });

    const match = await bcrypt.compare(device_secret, device.device_secret);
    if (!match)
        return res.status(401).json({ message: "Invalid device secret" });

    req.user_id = device.user_id;
    next();
}

// ---------------- FLEX AUTH (JWT + Device) ----------------

async function flexAuth(req, res, next) {
    const token = req.headers.authorization?.split(" ")[1];
    if (token) {
        const { data, error } = await supabase.auth.getUser(token);
        if (!error && data.user) {
            req.user_id = data.user.id;
            return next();
        }
    }
    return deviceAuthenticate(req, res, next);
}

// ---------------- PAIRING ----------------

// Device start pairing
app.post("/device/start-pair", async (req, res) => {
    const { device_id } = req.body;
    if (!device_id) return res.status(400).json({ message: "device_id required" });

    const pairing_code = Math.floor(100000 + Math.random() * 900000).toString();
    const expires_at = new Date(Date.now() + 5 * 60 * 1000);

    await supabase.from("device_pairing").upsert({
        device_id,
        pairing_code,
        expires_at,
    });

    res.json({ pairing_code });
});

// User confirms pairing (must be logged in)
app.post("/device/confirm-pair", authenticateUser, async (req, res) => {
    const { pairing_code } = req.body;

    const { data: pairing } = await supabase
        .from("device_pairing")
        .select("*")
        .eq("pairing_code", pairing_code)
        .single();

    if (!pairing)
        return res.status(400).json({ message: "Invalid pairing code" });

    if (new Date() > new Date(pairing.expires_at))
        return res.status(400).json({ message: "Code expired" });

    const deviceSecretPlain = crypto.randomUUID();
    const hashedSecret = await bcrypt.hash(deviceSecretPlain, 10);

    await supabase.from("devices").insert({
        user_id: req.user.id,
        device_id: pairing.device_id,
        device_secret: hashedSecret,
        device_secret_pending: deviceSecretPlain // geçici kolon
    });

    await supabase
        .from("device_pairing")
        .delete()
        .eq("device_id", pairing.device_id);

    res.json({ message: "Device linked" });
});

// Device polls for secret after pairing
app.post("/device/poll-pairing", async (req, res) => {
    const { device_id } = req.body;

    const { data: device } = await supabase
        .from("devices")
        .select("device_secret_pending")
        .eq("device_id", device_id)
        .single();

    if (!device)
        return res.json({ paired: false });

    if (!device.device_secret_pending)
        return res.json({ paired: true, device_secret: null });

    const secret = device.device_secret_pending;

    await supabase.from("devices")
        .update({ device_secret_pending: null })
        .eq("device_id", device_id);

    res.json({
        paired: true,
        device_secret: secret
    });
});

// ---------------- SPOTIFY OAUTH ----------------

app.get("/spotify/oauth", authenticateUser, async (req, res) => {
    const state = crypto.randomUUID();

    await supabase.from("oauth_states").insert({
        state,
        user_id: req.user.id,
        created_at: new Date(),
    });

    const scope = encodeURIComponent(
        "user-read-playback-state user-modify-playback-state user-read-currently-playing"
    );

    const url = `https://accounts.spotify.com/authorize?client_id=${process.env.SPOTIFY_CLIENT_ID
        }&response_type=code&redirect_uri=${encodeURIComponent(
            process.env.SPOTIFY_REDIRECT_URI
        )}&scope=${scope}&state=${state}`;

    res.redirect(url);
});

app.get("/spotify/callback", async (req, res) => {
    const { code, state } = req.query;

    const { data: stateEntry } = await supabase
        .from("oauth_states")
        .select("*")
        .eq("state", state)
        .single();

    if (!stateEntry) return res.status(400).send("Invalid state");

    const tokenRes = await fetch("https://accounts.spotify.com/api/token", {
        method: "POST",
        headers: {
            "Content-Type": "application/x-www-form-urlencoded",
            Authorization:
                "Basic " +
                Buffer.from(
                    process.env.SPOTIFY_CLIENT_ID +
                    ":" +
                    process.env.SPOTIFY_CLIENT_SECRET
                ).toString("base64"),
        },
        body: new URLSearchParams({
            code,
            redirect_uri: process.env.SPOTIFY_REDIRECT_URI,
            grant_type: "authorization_code",
        }),
    });

    const tokens = await tokenRes.json();
    if (!tokens.access_token)
        return res.status(400).send("Failed to get token");

    await supabase.from("spotify_tokens").upsert({
        user_id: stateEntry.user_id,
        access_token: tokens.access_token,
        refresh_token: tokens.refresh_token,
        expires_at: new Date(Date.now() + tokens.expires_in * 1000),
    });

    await supabase.from("oauth_states").delete().eq("state", state);

    const frontendUrl = process.env.FRONTEND_URL;
    if (!frontendUrl) return res.status(500).send("FRONTEND_URL not configured");
    res.redirect(`${frontendUrl}/index.html?spotify=connected`);
});

// ---------------- STATUS ENDPOINTS ----------------

app.get("/spotify/status", authenticateUser, async (req, res) => {
    try {
        const { data } = await supabase
            .from("spotify_tokens")
            .select("user_id")
            .eq("user_id", req.user.id)
            .single();

        res.json({ connected: !!data });
    } catch {
        res.json({ connected: false });
    }
});

app.get("/user/devices", authenticateUser, async (req, res) => {
    try {
        const { data: devices } = await supabase
            .from("devices")
            .select("device_id, created_at")
            .eq("user_id", req.user.id);

        res.json({ devices: devices || [] });
    } catch (err) {
        res.status(500).json({ message: err.message });
    }
});

// ---------------- TOKEN HELPER ----------------

async function getValidSpotifyToken(user_id) {
    const { data: spotify } = await supabase
        .from("spotify_tokens")
        .select("*")
        .eq("user_id", user_id)
        .single();

    if (!spotify) throw new Error("Spotify not linked");

    if (new Date() > new Date(spotify.expires_at)) {
        const tokenRes = await fetch("https://accounts.spotify.com/api/token", {
            method: "POST",
            headers: {
                "Content-Type": "application/x-www-form-urlencoded",
                Authorization:
                    "Basic " +
                    Buffer.from(
                        process.env.SPOTIFY_CLIENT_ID +
                        ":" +
                        process.env.SPOTIFY_CLIENT_SECRET
                    ).toString("base64"),
            },
            body: new URLSearchParams({
                grant_type: "refresh_token",
                refresh_token: spotify.refresh_token,
            }),
        });

        const tokens = await tokenRes.json();
        if (!tokens.access_token)
            throw new Error("Failed to refresh token");

        await supabase.from("spotify_tokens").update({
            access_token: tokens.access_token,
            refresh_token: tokens.refresh_token ?? spotify.refresh_token,
            expires_at: new Date(Date.now() + tokens.expires_in * 1000),
        }).eq("user_id", user_id);

        return tokens.access_token;
    }

    return spotify.access_token;
}

// ---------------- SPOTIFY CONTROL ----------------

app.post("/controls/next", flexAuth, async (req, res) => {
    try {
        const user_id = req.user_id;

        const accessToken = await getValidSpotifyToken(user_id);

        await fetch("https://api.spotify.com/v1/me/player/next", {
            method: "POST",
            headers: {
                Authorization: `Bearer ${accessToken}`,
            },
        });

        res.json({ success: true });
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});

app.post("/controls/previous", flexAuth, async (req, res) => {
    try {
        const user_id = req.user_id;

        const accessToken = await getValidSpotifyToken(user_id);

        await fetch("https://api.spotify.com/v1/me/player/previous", {
            method: "POST",
            headers: {
                Authorization: `Bearer ${accessToken}`,
            },
        });

        res.json({ success: true });
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});

app.put("/controls/play", flexAuth, async (req, res) => {
    try {
        const user_id = req.user_id;

        const accessToken = await getValidSpotifyToken(user_id);

        await fetch("https://api.spotify.com/v1/me/player/play", {
            method: "PUT",
            headers: {
                Authorization: `Bearer ${accessToken}`,
            },
        });

        res.json({ success: true });
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});

app.put("/controls/pause", flexAuth, async (req, res) => {
    try {
        const user_id = req.user_id;

        const accessToken = await getValidSpotifyToken(user_id);

        await fetch("https://api.spotify.com/v1/me/player/pause", {
            method: "PUT",
            headers: {
                Authorization: `Bearer ${accessToken}`,
            },
        });

        res.json({ success: true });
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});

app.get("/get-current-track-details", flexAuth, async (req, res) => {
    try {
        const user_id = req.user_id;

        const accessToken = await getValidSpotifyToken(user_id);

        const response = await fetch(
            "https://api.spotify.com/v1/me/player/currently-playing",
            {
                headers: {
                    Authorization: `Bearer ${accessToken}`,
                },
            }
        );

        if (response.status === 204)
            return res.json({ success: true, is_playing: false });

        const data = await response.json();

        res.json({
            success: true,
            is_playing: data.is_playing,
            device_name: data.device?.name,
            device_type: data.device?.type,
            volume_percent: data.device?.volume_percent,
            progress_ms: data.progress_ms,
            track_id: data.item?.id
        });
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});

app.get("/get-track-details", flexAuth, async (req, res) => {
    try {
        const user_id = req.user_id;

        const accessToken = await getValidSpotifyToken(user_id);

        const response = await fetch(
            "https://api.spotify.com/v1/me/player/currently-playing",
            {
                headers: {
                    Authorization: `Bearer ${accessToken}`,
                },
            }
        );

        if (response.status === 204)
            return res.json({ success: true, is_playing: false });

        const data = await response.json();

        res.json({
            success: true,
            track_name: data.item?.name,
            artist_name: data.item?.artists?.[0]?.name,
            album_name: data.item?.album?.name,
            album_image: data.item?.album?.images?.[0]?.url,
            duration_ms: data.item?.duration_ms,
            track_id: data.item?.id
        });
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});

app.get("/get-album-cover", flexAuth, async (req, res) => {
    try {
        const user_id = req.user_id;

        const accessToken = await getValidSpotifyToken(user_id);

        const response = await fetch(
            "https://api.spotify.com/v1/me/player/currently-playing",
            {
                headers: {
                    Authorization: `Bearer ${accessToken}`,
                },
            }
        );

        if (response.status === 204)
            return res.status(404).send("Nothing playing");

        const data = await response.json();
        const imageUrl = data.item?.album?.images?.[0]?.url;

        if (!imageUrl)
            return res.status(404).send("No album cover");

        const imageResponse = await fetch(imageUrl);
        const imageBuffer = await imageResponse.arrayBuffer();

        const { data: rawData } = await sharp(Buffer.from(imageBuffer))
            .resize(180, 180)
            .raw()
            .toBuffer({ resolveWithObject: true });

        const rgb565Buffer = Buffer.alloc(180 * 180 * 2);

        for (let i = 0, j = 0; i < rawData.length; i += 3, j += 2) {
            const r = rawData[i] >> 3;
            const g = rawData[i + 1] >> 2;
            const b = rawData[i + 2] >> 3;

            const rgb565 = (r << 11) | (g << 5) | b;

            rgb565Buffer[j] = rgb565 >> 8;
            rgb565Buffer[j + 1] = rgb565 & 0xff;
        }

        res.set({
            "Content-Type": "application/octet-stream",
            "Content-Length": rgb565Buffer.length.toString()
        });
        res.send(rgb565Buffer);
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});



app.listen(3000, () =>
    console.log("Server running on port 3000")
);
