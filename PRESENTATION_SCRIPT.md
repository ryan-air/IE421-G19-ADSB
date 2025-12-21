# ADS-B Aircraft Tracking System - Presentation Script

**Presenters:** Saksham Gupta & Apoorv Sagar Pitta  
**Course:** IE 421 - University of Illinois Urbana-Champaign  
**Advisor:** Professor David Lariviere  
**Duration:** ~10-12 minutes

---

## 🎬 OPENING HOOK [30 seconds]

*[Start with the 3D view on screen, aircraft flying across the globe]*

> "Right now, as we speak, there are over 5,000 aircraft in the sky above the United States. Every single one of them is broadcasting its exact location, speed, and altitude—completely unencrypted—for anyone to receive.
>
> We built a system that captures those signals from the air, processes over 5,000 data points per second, and visualizes them in real-time on this 3D globe you see here."

*[Pause to let the visual impact sink in]*

---

## 📋 THE PROBLEM [1.5 minutes]

> "In February 2023, a complete air traffic control system failure grounded thousands of flights nationwide. This incident highlighted a critical vulnerability: our aviation infrastructure is centralized and fragile.
>
> Current collision avoidance systems like TCAS cost between $50,000 and $150,000 per aircraft—completely out of reach for general aviation pilots, drone operators, and researchers.
>
> **Our question was simple:** Can we build a sophisticated aircraft tracking and collision prediction system using only commodity hardware?
>
> **The answer:** Yes. And we did it for under $100."

*[Show the Raspberry Pi and RTL-SDR dongle if available, or show a photo]*

> "This $35 Raspberry Pi and $25 software-defined radio is our entire hardware setup. It receives ADS-B signals that aircraft broadcast on 1090 MHz—signals that travel up to 250 nautical miles."

---

## 🛩️ LIVE DEMO - 2D MAP VIEW [3 minutes]

*[Switch to the 2D map view: http://localhost:3000 or your Railway URL]*

### Aircraft Tracking

> "Here's our 2D tracking interface. Each icon represents a real aircraft broadcasting right now. The cyan icons are airplanes, and these yellow ones are helicopters."

*[Click on an aircraft to show the info panel]*

> "When I click on this aircraft—let's see, this is [READ CALLSIGN]—we can see its exact details:
> - Altitude: [READ] feet
> - Ground speed: [READ] knots  
> - Heading: [READ] degrees
> - And its unique ICAO hex identifier: [READ]
>
> This data updates every 2 seconds with smooth animation—notice how the icons glide to their new positions rather than jumping."

### Flight Trails

*[Toggle on trails for one aircraft]*

> "I can toggle on the flight trail to see where this aircraft has been over the last two hours. Each dot represents a recorded position. You can actually see the turns it made during approach or departure."

### Prediction Wedges

*[Point to the colored wedges in front of aircraft]*

> "These colored wedges are our trajectory prediction system. They show where we predict each aircraft will be in the next 5 minutes using a Kalman filter algorithm. The color indicates altitude:
> - Green means low altitude—typically approaching or departing
> - Yellow is mid-altitude
> - Orange is cruise altitude, above 25,000 feet"

### Statistics Dashboard

*[Open the statistics panel]*

> "Our statistics dashboard shows real-time analytics:
> - Total flights currently tracked: [READ NUMBER]
> - Average speed across all aircraft: [READ] knots
> - Average altitude: [READ] feet
>
> And this altitude distribution histogram shows us how traffic is distributed—you can see most aircraft are at cruise altitude right now."

---

## 🌍 LIVE DEMO - 3D GLOBE VIEW [2 minutes]

*[Switch to 3D view: /3d.html]*

> "Now let's look at our immersive 3D visualization. This is built on Cesium.js with real terrain data from around the world."

*[Rotate the globe, zoom in on the Chicago area]*

> "I can orbit around the globe, zoom in to see individual aircraft... notice how they're rendered with actual 3D models at their correct altitudes. The helicopter here actually uses a helicopter 3D model."

*[Click on an aircraft to show trail]*

> "When I select an aircraft, we render its flight trail in 3D—you can actually see the altitude changes as it climbs or descends."

*[Show probability cones if visible]*

> "The probability cones are rendered as 3D volumes here, showing the expanding uncertainty in our trajectory predictions over time."

*[Demonstrate touch controls if on a tablet, or show the control buttons]*

> "We've also implemented full touch controls for tablets—pinch to zoom, two-finger drag to tilt—making this a practical tool for pilots or operators in the field."

---

## ⚙️ TECHNICAL DEEP DIVE [2.5 minutes]

*[Can show architecture diagram from report or just speak to it]*

> "Let me walk you through what's happening behind the scenes."

### Data Pipeline

> "Aircraft broadcast ADS-B messages twice per second. Our Raspberry Pi receives these signals via the RTL-SDR radio and decode them using dump1090—an open-source Mode S decoder.
>
> The decoded messages flow into our Python streaming service, which parses the data, validates it, and buffers it for bulk insertion."

### The Performance Challenge

> "Here's where it gets interesting. In our initial implementation, we used individual INSERT statements for each position update. With 100 aircraft transmitting every second, PostgreSQL couldn't keep up—we had 2-second write latency and the database was choking."

*[Pause for emphasis]*

> "We solved this with PostgreSQL's CSV COPY method for bulk loading. Instead of 100 individual transactions, we batch everything and send it in a single bulk operation.
>
> **The result: 100x performance improvement.** We went from 50 writes per second to over 5,000 writes per second. CPU usage dropped from 80% to 5%."

### Kalman Filter Prediction

> "For collision detection, we implemented a Kalman filter—the same algorithm NASA uses for spacecraft navigation.
>
> The filter tracks each aircraft in 3D space, predicts its trajectory 5 minutes into the future, and calculates expanding uncertainty bounds. When two aircraft's probability cones intersect, we flag a potential collision.
>
> This gives pilots a 5-minute advance warning with 95% confidence intervals—compared to TCAS which only provides 25-40 seconds of warning."

---

## 🔧 CHALLENGES WE OVERCAME [1.5 minutes]

> "Building this system wasn't straightforward. Let me share three key challenges we solved:"

### Challenge 1: Supabase Connection Limits

> "Supabase's free tier limits database connections to around 15. Our Raspberry Pi and web server were fighting for connections, causing intermittent failures.
>
> **Solution:** Conservative connection pooling—just 3 connections per service—with intelligent caching that achieved 75% cache hit rates. Now the system runs for weeks without restart."

### Challenge 2: Jerky Animations

> "Aircraft icons were jumping to new positions every 2 seconds—it looked terrible.
>
> **Solution:** Custom animation interpolation using requestAnimationFrame at 60 FPS with cubic easing. We even handle heading wrap-around at 0/360 degrees correctly."

### Challenge 3: Data Integrity

> "ADS-B is unencrypted and unauthenticated. We received garbage data constantly—corrupted messages, impossible altitudes, aircraft at coordinates 0,0.
>
> **Solution:** Multi-layer validation. Every message is checked for protocol structure, type-safe field parsing, and range validation. The system now runs 30+ days without crashes."

---

## 🎯 IMPACT & USE CASES [1 minute]

> "Who benefits from this system?
>
> - **Small airports** without radar can now monitor local traffic
> - **Drone operators** can identify manned aircraft in their area for safe separation
> - **Aviation researchers** can study traffic patterns and airspace utilization
> - **Emergency responders** can track aircraft in remote search-and-rescue areas
> - **Educators** can demonstrate real-time physics—velocity, acceleration, trajectory mathematics—with live data
>
> All of this for under $100 in hardware and zero recurring costs."

---

## 🔮 FUTURE WORK [30 seconds]

> "What's next?
>
> - **WebSocket support** would eliminate polling and enable true millisecond-latency updates
> - **Machine learning trajectory prediction** could anticipate turns and maneuvers, reducing false collision alerts
> - **Flight plan integration** would dramatically improve prediction accuracy
> - And a **mobile app** would put this capability in every pilot's pocket"

---

## 🏁 CONCLUSION [30 seconds]

> "We set out to prove that sophisticated aircraft tracking doesn't require million-dollar infrastructure. With a Raspberry Pi, a $25 radio, and open-source software, we built a system that:
>
> - Tracks aircraft in real-time across 250 nautical miles
> - Predicts trajectories 5 minutes into the future
> - Processes 5,000+ data points per second
> - Visualizes everything in both 2D and immersive 3D
>
> As aviation continues to grow—and drones enter the airspace—accessible monitoring tools like this become increasingly critical.
>
> **Thank you. We're happy to take questions.**"

---

## 📝 Q&A PREPARATION

### Likely Questions & Answers

**Q: How accurate is the position data?**
> "ADS-B position accuracy depends on the aircraft's GPS, typically 10-50 meters horizontal. Altitude is pressure-based, not GPS, referenced to standard pressure at 29.92 inches of mercury."

**Q: What about aircraft without ADS-B?**
> "ADS-B Out was mandated by the FAA in January 2020 for most controlled airspace. Older aircraft without ADS-B won't appear in our system. A future enhancement would be MLAT—multilateration—using multiple receivers to triangulate non-ADS-B aircraft."

**Q: Can this be spoofed?**
> "Yes, ADS-B has no authentication. Anyone with a software-defined radio could broadcast fake aircraft. This is a known security concern—not something we solve, but something the industry is actively researching."

**Q: What's the range?**
> "Line-of-sight, typically 150-250 nautical miles for aircraft at altitude. Ground-level aircraft or those behind terrain may not be detected."

**Q: How does this compare to FlightAware or FlightRadar24?**
> "Those services aggregate data from thousands of receivers worldwide. Our system demonstrates the same core technology but runs entirely locally—no data leaves our infrastructure. It's self-contained and doesn't require internet to function between the Pi and the viewer."

**Q: What's the collision detection false positive rate?**
> "We haven't quantified it precisely, but false positives are common during approach/departure when aircraft are on converging paths that ATC will separate. The Kalman filter assumes constant velocity, which breaks down during maneuvers."

**Q: Why PostgreSQL instead of a time-series database?**
> "Great question. A time-series database like TimescaleDB or InfluxDB would be more efficient for the position history. We used PostgreSQL via Supabase for rapid prototyping and the generous free tier. Migrating to TimescaleDB is on our future roadmap."

---

## 🖥️ DEMO CHECKLIST

Before presenting, verify:

- [ ] ADS-B streamer is running on Raspberry Pi
- [ ] Web server is running (locally or on Railway)
- [ ] Aircraft are appearing on the map (need active ADS-B traffic)
- [ ] 3D view loads without errors
- [ ] Statistics panel shows data
- [ ] Trails toggle works
- [ ] Internet connection is stable (if using cloud database)
- [ ] Backup screenshots/video ready in case of live demo failure

### Backup Plan

If the live demo fails:
1. Show pre-recorded video of the system working
2. Use screenshots from busy traffic periods
3. Walk through the architecture diagram instead

---

## 🎥 SUGGESTED VISUAL AIDS

1. **Opening:** 3D globe with aircraft flying
2. **Problem slide:** Photo of ATC outage news headlines
3. **Hardware:** Photo of Raspberry Pi + RTL-SDR setup
4. **Architecture:** System diagram from report
5. **Performance:** Before/after metrics comparison
6. **Live demo:** Actual running system
7. **Closing:** Summary bullet points

---

*Good luck with your presentation! 🛩️*

