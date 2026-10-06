document.addEventListener("DOMContentLoaded", function () {
  var gallery = document.getElementById("gallery");
  var overlay = document.getElementById("videoOverlay");
  var fsVideo = document.getElementById("fullscreenVideo");
  if (!gallery || !overlay || !fsVideo) return;

  gallery.addEventListener("click", function (e) {
    var item = e.target.closest(".media-item");
    if (!item || item.dataset.type === "image") return;

    var src = item.getAttribute("data-src");
    fsVideo.src = src;
    overlay.style.display = "flex";
    fsVideo.play();

    if (overlay.requestFullscreen) {
      overlay.requestFullscreen();
    } else if (overlay.webkitRequestFullscreen) {
      overlay.webkitRequestFullscreen();
    }
  });

  function exitHandler() {
    var isFullscreen = document.fullscreenElement || document.webkitFullscreenElement;
    if (!isFullscreen) {
      overlay.style.display = "none";
      fsVideo.pause();
      fsVideo.src = "";
    }
  }

  document.addEventListener("fullscreenchange", exitHandler);
  document.addEventListener("webkitfullscreenchange", exitHandler);

  overlay.addEventListener("click", function () {
    if (document.exitFullscreen) {
      document.exitFullscreen();
    } else if (document.webkitExitFullscreen) {
      document.webkitExitFullscreen();
    }
  });
});
