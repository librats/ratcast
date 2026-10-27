import { MotionConfig } from "motion/react";
import { Developers } from "./components/Developers";
import { Download } from "./components/Download";
import { Features } from "./components/Features";
import { Footer } from "./components/Footer";
import { Hero } from "./components/Hero";
import { HowItWorks } from "./components/HowItWorks";
import { Marquee, Stats } from "./components/Marquee";
import { Nav } from "./components/Nav";
import { Topology } from "./components/Topology";

export default function App() {
  return (
    <MotionConfig reducedMotion="user">
      <div aria-hidden className="grain pointer-events-none fixed inset-0 z-[100] opacity-[0.035] mix-blend-overlay" />
      <Nav />
      <main>
        <Hero />
        <Marquee />
        <Stats />
        <Features />
        <HowItWorks />
        <Topology />
        <Developers />
        <Download />
      </main>
      <Footer />
    </MotionConfig>
  );
}
