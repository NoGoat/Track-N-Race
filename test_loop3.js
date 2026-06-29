let timesArray = new Float32Array([1.0, 1.016, 1.032, 1.048, 1.064]);
let playbackIndex = 0;
let virtualTime = 1.0;
let lastUpdateRealTime = 0;

function play() {
  lastUpdateRealTime = performance.now();
  playbackLoop();
}

function playbackLoop() {
  const now = performance.now();
  const deltaRealSec = (now - lastUpdateRealTime) / 1000;
  lastUpdateRealTime = now;
  
  virtualTime += deltaRealSec * 1;
  
  let endIndex = playbackIndex;
  while (endIndex < timesArray.length && timesArray[endIndex] <= virtualTime) {
    endIndex++;
  }
  
  console.log("endIndex:", endIndex, "virtualTime:", virtualTime);
  playbackIndex = endIndex;
  
  if (playbackIndex < timesArray.length) {
    setTimeout(playbackLoop, 16);
  }
}

play();
