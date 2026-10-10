document.addEventListener('DOMContentLoaded', () => {
    // Mobile Navigation Toggle
    const mobileToggleBtn = document.getElementById('mobileToggleBtn');
    const navMenu = document.querySelector('.nav-menu');

    if (mobileToggleBtn && navMenu) {
        mobileToggleBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            const isOpen = navMenu.classList.toggle('mobile-active');
            mobileToggleBtn.setAttribute('aria-expanded', isOpen ? 'true' : 'false');
        });

        // Close menu on outside click
        document.addEventListener('click', (e) => {
            if (navMenu.classList.contains('mobile-active') && !navMenu.contains(e.target) && !mobileToggleBtn.contains(e.target)) {
                navMenu.classList.remove('mobile-active');
                mobileToggleBtn.setAttribute('aria-expanded', 'false');
            }
        });

        // Close menu on link click
        navMenu.querySelectorAll('.nav-btn').forEach(btn => {
            btn.addEventListener('click', () => {
                navMenu.classList.remove('mobile-active');
                mobileToggleBtn.setAttribute('aria-expanded', 'false');
            });
        });
    }

    // Set active navigation button based on current URL path
    const navButtons = document.querySelectorAll('.nav-btn');
    // Normalise "/setup", "/setup/" and "/setup/index.html" to "/setup"
    const normalisePath = path => path.toLowerCase()
        .replace(/(index)?(\.html)?$/, '')
        .replace(/\/+$/, '');
    const currentPath = normalisePath(window.location.pathname);

    navButtons.forEach(btn => {
        const href = btn.getAttribute('href');
        if (!href) return;

        btn.classList.toggle('active', normalisePath(href) === currentPath);
    });

    // Carousel Logic
    const viewport = document.querySelector('.carousel-viewport');
    const track = document.querySelector('.carousel-track');
    const slides = Array.from(track?.children || []);
    const nextButton = document.querySelector('.carousel-btn.next');
    const prevButton = document.querySelector('.carousel-btn.prev');
    const dotsNav = document.querySelector('.carousel-indicators');
    const dots = Array.from(dotsNav?.children || []);

    if (viewport && track && slides.length > 0) {
        let currentSlideIndex = 0;
        const gap = 48; // Physical buffer gap between slides in px

        const updateCarousel = (index, animate = true) => {
            currentSlideIndex = ((index % slides.length) + slides.length) % slides.length;
            const viewportWidth = viewport.getBoundingClientRect().width;
            if (viewportWidth === 0) return;

            slides.forEach((slide, i) => {
                slide.style.width = `${viewportWidth}px`;
                slide.classList.toggle('active', i === currentSlideIndex);
            });

            const offset = currentSlideIndex * (viewportWidth + gap);
            track.style.transition = animate ? 'transform 0.45s cubic-bezier(0.25, 1, 0.5, 1)' : 'none';
            track.style.transform = `translate3d(-${offset}px, 0, 0)`;

            dots.forEach((dot, i) => {
                dot.classList.toggle('active', i === currentSlideIndex);
            });
        };

        updateCarousel(0, false);

        // Keep dimensions perfectly synced on resize and zoom change
        if (window.ResizeObserver) {
            const ro = new ResizeObserver(() => {
                updateCarousel(currentSlideIndex, false);
            });
            ro.observe(viewport);
        } else {
            window.addEventListener('resize', () => updateCarousel(currentSlideIndex, false));
        }

        const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

        const resetAutoPlay = () => {
            clearInterval(autoPlayInterval);
            if (reduceMotion) return;
            autoPlayInterval = setInterval(() => {
                updateCarousel(currentSlideIndex + 1);
            }, 5000);
        };

        nextButton?.addEventListener('click', () => {
            updateCarousel(currentSlideIndex + 1);
            resetAutoPlay();
        });

        prevButton?.addEventListener('click', () => {
            updateCarousel(currentSlideIndex - 1);
            resetAutoPlay();
        });

        dotsNav?.addEventListener('click', e => {
            const targetDot = e.target.closest('.dot');
            if (!targetDot) return;
            const index = dots.indexOf(targetDot);
            if (index !== -1) {
                updateCarousel(index);
                resetAutoPlay();
            }
        });

        // Touch Swipe Support for Mobile
        let touchStartX = 0;
        let touchStartY = 0;
        let touchEndX = 0;
        let touchEndY = 0;

        track.addEventListener('touchstart', (e) => {
            touchStartX = e.changedTouches[0].clientX;
            touchStartY = e.changedTouches[0].clientY;
            clearInterval(autoPlayInterval);
        }, { passive: true });

        track.addEventListener('touchend', (e) => {
            touchEndX = e.changedTouches[0].clientX;
            touchEndY = e.changedTouches[0].clientY;
            const diffX = touchStartX - touchEndX;
            const diffY = touchStartY - touchEndY;

            // Only trigger horizontal swipe if horizontal movement is greater than vertical movement
            if (Math.abs(diffX) > Math.abs(diffY) && Math.abs(diffX) > 35) {
                if (diffX > 0) {
                    // Swiped Left -> Next Slide
                    updateCarousel(currentSlideIndex + 1);
                } else {
                    // Swiped Right -> Prev Slide
                    updateCarousel(currentSlideIndex - 1);
                }
            }
            resetAutoPlay();
        }, { passive: true });

        // Auto-play interval
        let autoPlayInterval = reduceMotion ? null : setInterval(() => {
            updateCarousel(currentSlideIndex + 1);
        }, 5000);

        // Pause on hover
        const carouselContainer = document.querySelector('.carousel-container');
        carouselContainer?.addEventListener('mouseenter', () => clearInterval(autoPlayInterval));
        carouselContainer?.addEventListener('mouseleave', () => resetAutoPlay());
    }

    // Module switcher ("Everything else"): one tab per module, one visible screenshot
    const moduleTabs = Array.from(document.querySelectorAll('.module-tab'));
    const modulePanes = Array.from(document.querySelectorAll('.module-pane'));

    if (moduleTabs.length > 0 && moduleTabs.length === modulePanes.length) {
        const moduleList = moduleTabs[0].parentElement;

        const selectModule = (index, focus = false) => {
            moduleTabs.forEach((tab, i) => {
                const isActive = i === index;
                tab.classList.toggle('active', isActive);
                tab.setAttribute('aria-selected', isActive ? 'true' : 'false');
                tab.tabIndex = isActive ? 0 : -1;
                modulePanes[i].classList.toggle('active', isActive);
                if (isActive) {
                    modulePanes[i].removeAttribute('aria-hidden');
                } else {
                    modulePanes[i].setAttribute('aria-hidden', 'true');
                }
            });

            const tab = moduleTabs[index];
            if (focus) tab.focus({ preventScroll: true });

            // On phones the list is a horizontal strip: keep the active tab centred in it
            if (moduleList.scrollWidth > moduleList.clientWidth) {
                moduleList.scrollTo({
                    left: tab.offsetLeft - (moduleList.clientWidth - tab.offsetWidth) / 2,
                    behavior: 'smooth'
                });
            }
        };

        moduleTabs.forEach((tab, i) => {
            tab.addEventListener('click', () => selectModule(i));

            tab.addEventListener('keydown', (e) => {
                const step = { ArrowDown: 1, ArrowRight: 1, ArrowUp: -1, ArrowLeft: -1 }[e.key];
                let target = null;
                if (step) target = (i + step + moduleTabs.length) % moduleTabs.length;
                else if (e.key === 'Home') target = 0;
                else if (e.key === 'End') target = moduleTabs.length - 1;
                if (target === null) return;
                e.preventDefault();
                selectModule(target, true);
            });
        });
    }

    // Lightbox Logic & Fluid GPU Zoom & Pan Algorithm
    const lightbox = document.getElementById('lightbox');
    const lightboxImg = lightbox?.querySelector('img');
    const lightboxClose = document.getElementById('lightboxClose');
    const clickableImages = document.querySelectorAll('.mockup-img');

    if (lightbox && lightboxImg) {
        let currentScale = 1;
        let translateX = 0;
        let translateY = 0;
        let isDragging = false;
        let isMoved = false;
        let startX = 0;
        let startY = 0;
        let initialTranslateX = 0;
        let initialTranslateY = 0;

        const applyTransform = (animate = true) => {
            lightboxImg.style.transition = animate ? 'transform 0.3s cubic-bezier(0.16, 1, 0.3, 1)' : 'none';
            lightboxImg.style.transform = `translate3d(${translateX}px, ${translateY}px, 0) scale(${currentScale})`;
        };

        const clampPan = () => {
            if (currentScale <= 1) {
                translateX = 0;
                translateY = 0;
                return;
            }
            const scaledW = (lightboxImg.offsetWidth || window.innerWidth * 0.9) * currentScale;
            const scaledH = (lightboxImg.offsetHeight || window.innerHeight * 0.88) * currentScale;
            const maxPanX = Math.max(0, (scaledW - window.innerWidth) / 2 + 50);
            const maxPanY = Math.max(0, (scaledH - window.innerHeight) / 2 + 50);

            translateX = Math.min(maxPanX, Math.max(-maxPanX, translateX));
            translateY = Math.min(maxPanY, Math.max(-maxPanY, translateY));
        };

        let lightboxLoadId = 0;

        const openLightbox = (src, previewSrc) => {
            // Show the already-loaded card image instantly so the previous image never flashes,
            // then swap in the full-size file once it has downloaded and decoded.
            const loadId = ++lightboxLoadId;
            lightboxImg.src = previewSrc || src;
            if (previewSrc && previewSrc !== src) {
                const full = new Image();
                full.src = src;
                full.decode()
                    .catch(() => {})
                    .then(() => {
                        if (loadId === lightboxLoadId) lightboxImg.src = src;
                    });
            }
            currentScale = 1;
            translateX = 0;
            translateY = 0;
            lightbox.classList.remove('is-zoomed', 'is-dragging');
            applyTransform(false);
            lightbox.classList.add('active');
            document.documentElement.classList.add('modal-open');
            document.body.classList.add('modal-open');
            document.documentElement.style.overflow = 'hidden';
            document.body.style.overflow = 'hidden';
        };

        const closeLightbox = () => {
            lightboxLoadId++;
            lightbox.classList.remove('active', 'is-zoomed', 'is-dragging');
            currentScale = 1;
            translateX = 0;
            translateY = 0;
            applyTransform(false);
            document.documentElement.classList.remove('modal-open');
            document.body.classList.remove('modal-open');
            document.documentElement.style.overflow = '';
            document.body.style.overflow = '';
        };

        clickableImages.forEach(wrapper => {
            wrapper.addEventListener('click', () => {
                const img = wrapper.tagName === 'IMG' ? wrapper : wrapper.querySelector('img');
                if (img && img.src) {
                    // If the browser picked the AVIF source, open its largest (full-size) candidate;
                    // otherwise fall back to the full-size WebP in <img src>.
                    const avifSource = img.closest('picture')?.querySelector('source[type="image/avif"]');
                    const fullAvif = avifSource && img.currentSrc.endsWith('.avif')
                        ? avifSource.srcset.split(',').pop().trim().split(' ')[0]
                        : null;
                    openLightbox(fullAvif || img.src, img.currentSrc || img.src);
                }
            });
        });

        lightboxClose?.addEventListener('click', (e) => {
            e.stopPropagation();
            closeLightbox();
        });

        // Close on background click (when clicking outside the image)
        lightbox.addEventListener('click', (e) => {
            if (e.target === lightbox || e.target.id === 'lightboxScroll' || e.target.id === 'lightboxCenterer') {
                closeLightbox();
            }
        });

        // ESC key to close
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && lightbox.classList.contains('active')) {
                closeLightbox();
            }
        });

        // Click / Drag on image
        lightboxImg.addEventListener('dragstart', (e) => e.preventDefault());

        const handlePointerDown = (clientX, clientY) => {
            if (currentScale <= 1) return;
            isDragging = true;
            isMoved = false;
            startX = clientX;
            startY = clientY;
            initialTranslateX = translateX;
            initialTranslateY = translateY;
            lightbox.classList.add('is-dragging');
            lightboxImg.style.transition = 'none';
        };

        const handlePointerMove = (clientX, clientY) => {
            if (!isDragging) return;
            const deltaX = clientX - startX;
            const deltaY = clientY - startY;
            if (Math.hypot(deltaX, deltaY) > 5) {
                isMoved = true;
            }
            translateX = initialTranslateX + deltaX;
            translateY = initialTranslateY + deltaY;
            clampPan();
            applyTransform(false);
        };

        const handlePointerUp = () => {
            if (!isDragging) return;
            isDragging = false;
            lightbox.classList.remove('is-dragging');
            applyTransform(true);
        };

        // Mouse Events
        lightboxImg.addEventListener('mousedown', (e) => {
            if (e.button !== 0) return;
            handlePointerDown(e.clientX, e.clientY);
        });

        window.addEventListener('mousemove', (e) => {
            handlePointerMove(e.clientX, e.clientY);
        });

        window.addEventListener('mouseup', () => {
            handlePointerUp();
        });

        // Touch Events
        lightboxImg.addEventListener('touchstart', (e) => {
            if (e.touches.length === 1) {
                handlePointerDown(e.touches[0].clientX, e.touches[0].clientY);
            }
        }, { passive: true });

        window.addEventListener('touchmove', (e) => {
            if (e.touches.length === 1) {
                handlePointerMove(e.touches[0].clientX, e.touches[0].clientY);
            }
        }, { passive: true });

        window.addEventListener('touchend', () => {
            handlePointerUp();
        });

        // Click to Zoom in / Zoom out
        lightboxImg.addEventListener('click', (e) => {
            e.stopPropagation();
            if (isMoved) {
                isMoved = false;
                return;
            }

            if (currentScale > 1) {
                // Smooth Zoom out to fitted view
                currentScale = 1;
                translateX = 0;
                translateY = 0;
                lightbox.classList.remove('is-zoomed');
                applyTransform(true);
            } else {
                // Smooth Zoom in centered directly on clicked coordinates
                const centerX = window.innerWidth / 2;
                const centerY = window.innerHeight / 2;
                const clickOffsetX = e.clientX - centerX;
                const clickOffsetY = e.clientY - centerY;

                currentScale = 2.2;
                translateX = -clickOffsetX * (currentScale - 1);
                translateY = -clickOffsetY * (currentScale - 1);
                clampPan();
                lightbox.classList.add('is-zoomed');
                applyTransform(true);
            }
        });

        // Mouse Wheel to Smooth Zoom
        lightbox.addEventListener('wheel', (e) => {
            if (!lightbox.classList.contains('active')) return;
            e.preventDefault();
            const zoomDelta = e.deltaY < 0 ? 1.2 : 0.8;
            const newScale = Math.min(Math.max(1, currentScale * zoomDelta), 4);
            if (Math.abs(newScale - currentScale) < 0.01) return;

            if (newScale <= 1.05) {
                currentScale = 1;
                translateX = 0;
                translateY = 0;
                lightbox.classList.remove('is-zoomed');
            } else {
                const centerX = window.innerWidth / 2;
                const centerY = window.innerHeight / 2;
                const mouseOffsetX = e.clientX - centerX;
                const mouseOffsetY = e.clientY - centerY;
                const scaleRatio = newScale / currentScale;

                translateX = mouseOffsetX - (mouseOffsetX - translateX) * scaleRatio;
                translateY = mouseOffsetY - (mouseOffsetY - translateY) * scaleRatio;
                currentScale = newScale;
                lightbox.classList.add('is-zoomed');
            }
            clampPan();
            applyTransform(true);
        }, { passive: false });
    }

    // License text, fetched once per file and shared by the pop-up and the credits reader
    const licenseCache = new Map();
    const loadLicense = (file) => {
        if (!licenseCache.has(file)) {
            licenseCache.set(file, fetch(`/assets/licenses/${file}`).then(response => {
                if (!response.ok) throw new Error('Failed to load license');
                return response.text();
            }).catch(error => {
                licenseCache.delete(file);
                throw error;
            }));
        }
        return licenseCache.get(file);
    };
    const licenseError = 'Error loading license text. Please try again later.';

    // License Modal Logic
    const licenseModal = document.getElementById('licenseModal');
    const closeLicenseModal = document.getElementById('closeLicenseModal');
    const licenseTextContent = document.getElementById('licenseTextContent');
    const licenseModalTitle = document.getElementById('licenseModalTitle');
    let licenseModalRequest = 0;

    const openLicenseModal = async (title, file) => {
        if (!licenseModal) return;
        const request = ++licenseModalRequest;
        licenseModalTitle.textContent = title;
        licenseTextContent.textContent = 'Loading...';
        licenseModal.classList.add('active');
        document.documentElement.classList.add('modal-open');
        document.body.classList.add('modal-open');
        document.documentElement.style.overflow = 'hidden';
        document.body.style.overflow = 'hidden';

        try {
            const text = await loadLicense(file);
            if (request === licenseModalRequest) licenseTextContent.textContent = text;
        } catch (error) {
            if (request === licenseModalRequest) licenseTextContent.textContent = licenseError;
            console.error(error);
        }
    };

    if (licenseModal) {
        // Footer GPL link (a non-button trigger, so it needs keyboard activation too)
        document.querySelectorAll('[data-license]').forEach(btn => {
            if (btn.tagName !== 'BUTTON') {
                btn.addEventListener('keydown', (e) => {
                    if (e.key === 'Enter' || e.key === ' ') {
                        e.preventDefault();
                        btn.click();
                    }
                });
            }
            btn.addEventListener('click', () => openLicenseModal('Track N Race License', btn.getAttribute('data-license')));
        });

        const closeModal = () => {
            licenseModal.classList.remove('active');
            document.documentElement.classList.remove('modal-open');
            document.body.classList.remove('modal-open');
            document.documentElement.style.overflow = '';
            document.body.style.overflow = '';
        };

        closeLicenseModal?.addEventListener('click', closeModal);

        licenseModal.addEventListener('click', (e) => {
            if (e.target === licenseModal) {
                closeModal();
            }
        });

        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && licenseModal.classList.contains('active')) {
                closeModal();
            }
        });
    }

    // Credits library browser: selecting a project shows its license in the reader.
    // On phones the list is a horizontal strip above the reader.
    const libraryItems = Array.from(document.querySelectorAll('.library-item'));
    const libraryReader = document.getElementById('library-reader');

    if (libraryItems.length > 0 && libraryReader) {
        const readerName = libraryReader.querySelector('.reader-name');
        const readerStack = libraryReader.querySelector('.reader-stack');
        const readerSpdx = libraryReader.querySelector('.reader-spdx');
        const readerLink = libraryReader.querySelector('.reader-link');
        const readerText = libraryReader.querySelector('.reader-text');
        const libraryList = libraryItems[0].closest('.library-list');
        const stackLabels = { core: 'Core', electron: 'Electron', qt: 'Qt' };
        let readerRequest = 0;

        const showInReader = async (item) => {
            libraryItems.forEach(other => {
                const isActive = other === item;
                other.classList.toggle('active', isActive);
                other.setAttribute('aria-pressed', isActive ? 'true' : 'false');
            });

            // Keep the selected project in view inside the list: centred in the phone strip,
            // scrolled into view in the desktop column (the page itself never scrolls)
            if (libraryList) {
                const listRect = libraryList.getBoundingClientRect();
                const itemRect = item.getBoundingClientRect();
                if (libraryList.scrollWidth > libraryList.clientWidth) {
                    libraryList.scrollBy({
                        left: itemRect.left - listRect.left - (listRect.width - itemRect.width) / 2,
                        behavior: 'smooth'
                    });
                } else if (itemRect.top < listRect.top || itemRect.bottom > listRect.bottom) {
                    libraryList.scrollBy({
                        top: itemRect.top - listRect.top - (listRect.height - itemRect.height) / 2,
                        behavior: 'smooth'
                    });
                }
            }

            const name = item.querySelector('.library-name').textContent;
            readerName.textContent = name;
            readerStack.dataset.stack = item.dataset.stack;
            readerStack.textContent = stackLabels[item.dataset.stack] || '';
            readerSpdx.textContent = item.dataset.spdx;
            readerLink.href = item.dataset.href;
            readerLink.setAttribute('aria-label', `${name} website`);

            const request = ++readerRequest;
            readerText.classList.add('is-loading');
            try {
                const text = await loadLicense(item.dataset.file);
                if (request !== readerRequest) return;
                readerText.textContent = text;
                readerText.scrollTop = 0;
            } catch (error) {
                if (request === readerRequest) readerText.textContent = licenseError;
                console.error(error);
            } finally {
                if (request === readerRequest) readerText.classList.remove('is-loading');
            }
        };

        libraryItems.forEach(item => {
            item.addEventListener('click', () => showInReader(item));
        });

        showInReader(libraryItems.find(item => item.classList.contains('active')) || libraryItems[0]);
    }

    // Setup Page Scenario Selector (Single PC vs Dual System)
    const scenarioSingleBtn = document.getElementById('scenarioSingleBtn');
    const scenarioDualBtn = document.getElementById('scenarioDualBtn');
    const scenarioSinglePane = document.getElementById('scenarioSinglePane');
    const scenarioDualPane = document.getElementById('scenarioDualPane');

    if (scenarioSingleBtn && scenarioDualBtn && scenarioSinglePane && scenarioDualPane) {
        scenarioSingleBtn.addEventListener('click', () => {
            scenarioSingleBtn.classList.add('active');
            scenarioSingleBtn.setAttribute('aria-selected', 'true');
            scenarioDualBtn.classList.remove('active');
            scenarioDualBtn.setAttribute('aria-selected', 'false');
            scenarioSinglePane.classList.add('active');
            scenarioDualPane.classList.remove('active');
        });

        scenarioDualBtn.addEventListener('click', () => {
            scenarioDualBtn.classList.add('active');
            scenarioDualBtn.setAttribute('aria-selected', 'true');
            scenarioSingleBtn.classList.remove('active');
            scenarioSingleBtn.setAttribute('aria-selected', 'false');
            scenarioDualPane.classList.add('active');
            scenarioSinglePane.classList.remove('active');
        });
    }

    // Setup Page Platform Tabs (Windows vs macOS vs Linux)
    const platformTabButtons = document.querySelectorAll('.platform-tab-btn');
    const platformPanes = document.querySelectorAll('.platform-pane');

    if (platformTabButtons.length > 0 && platformPanes.length > 0) {
        platformTabButtons.forEach(btn => {
            btn.addEventListener('click', () => {
                const targetPaneId = btn.getAttribute('aria-controls');
                if (!targetPaneId) return;

                platformTabButtons.forEach(b => {
                    b.classList.remove('active');
                    b.setAttribute('aria-selected', 'false');
                });
                platformPanes.forEach(p => {
                    p.classList.remove('active');
                });

                btn.classList.add('active');
                btn.setAttribute('aria-selected', 'true');
                const targetPane = document.getElementById(targetPaneId);
                if (targetPane) targetPane.classList.add('active');
            });
        });
    }

    // Copy to Clipboard Action Buttons
    const copyButtons = document.querySelectorAll('.copy-action-btn[data-copy]');
    copyButtons.forEach(btn => {
        btn.addEventListener('click', async (e) => {
            e.stopPropagation();
            const textToCopy = btn.getAttribute('data-copy');
            if (!textToCopy) return;

            try {
                if (navigator.clipboard && window.isSecureContext) {
                    await navigator.clipboard.writeText(textToCopy);
                } else {
                    const textArea = document.createElement('textarea');
                    textArea.value = textToCopy;
                    textArea.style.position = 'fixed';
                    textArea.style.opacity = '0';
                    document.body.appendChild(textArea);
                    textArea.focus();
                    textArea.select();
                    document.execCommand('copy');
                    textArea.remove();
                }

                const span = btn.querySelector('span');
                const origText = span ? span.textContent : 'Copy';
                btn.classList.add('copied');
                if (span) span.textContent = 'Copied!';

                setTimeout(() => {
                    btn.classList.remove('copied');
                    if (span) span.textContent = origText;
                }, 2000);
            } catch (err) {
                console.error('Failed to copy text: ', err);
            }
        });
    });
});
