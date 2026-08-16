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
    const currentPath = window.location.pathname.toLowerCase();
    const isHomePage = currentPath.endsWith('/') || currentPath.endsWith('index.html') || currentPath.endsWith('index') || currentPath === '';

    navButtons.forEach(btn => {
        const href = btn.getAttribute('href');
        if (!href) return;

        btn.classList.remove('active');
        const pageFileName = href.split('/').pop().toLowerCase();
        const pageBase = pageFileName.replace(/\.html$/, '');

        if ((pageBase === 'index' || pageBase === '') && isHomePage) {
            btn.classList.add('active');
        } else if (pageBase !== 'index' && pageBase !== '' && !isHomePage && currentPath.includes(pageBase)) {
            btn.classList.add('active');
        }
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

        const resetAutoPlay = () => {
            clearInterval(autoPlayInterval);
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
        let autoPlayInterval = setInterval(() => {
            updateCarousel(currentSlideIndex + 1);
        }, 5000);

        // Pause on hover
        const carouselContainer = document.querySelector('.carousel-container');
        carouselContainer?.addEventListener('mouseenter', () => clearInterval(autoPlayInterval));
        carouselContainer?.addEventListener('mouseleave', () => resetAutoPlay());
    }

    // Lightbox Logic & Fluid GPU Zoom & Pan Algorithm
    const lightbox = document.getElementById('lightbox');
    const lightboxImg = lightbox?.querySelector('img');
    const lightboxClose = document.getElementById('lightboxClose');
    const clickableImages = document.querySelectorAll('.card-img-wrapper, .mockup-img');

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

        const openLightbox = (src) => {
            lightboxImg.src = src;
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
                    openLightbox(img.src);
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

    // Smooth Accordion Animation for Attributions
    const accordions = document.querySelectorAll('.attribution-accordion');
    accordions.forEach(accordion => {
        const summary = accordion.querySelector('.attribution-section-title');
        if (!summary) return;

        let isAnimating = false;

        summary.addEventListener('click', (e) => {
            e.preventDefault();
            if (isAnimating) return;
            isAnimating = true;

            if (accordion.open) {
                const startHeight = accordion.offsetHeight;
                const endHeight = summary.offsetHeight;

                accordion.style.height = `${startHeight}px`;
                accordion.style.overflow = 'hidden';
                accordion.classList.add('is-collapsing');

                requestAnimationFrame(() => {
                    accordion.style.transition = 'height 0.35s cubic-bezier(0.16, 1, 0.3, 1)';
                    accordion.style.height = `${endHeight}px`;
                });

                setTimeout(() => {
                    accordion.open = false;
                    accordion.style.height = '';
                    accordion.style.overflow = '';
                    accordion.style.transition = '';
                    accordion.classList.remove('is-collapsing');
                    isAnimating = false;
                }, 350);
            } else {
                accordion.open = true;
                const endHeight = accordion.offsetHeight;
                const startHeight = summary.offsetHeight;

                accordion.style.height = `${startHeight}px`;
                accordion.style.overflow = 'hidden';

                requestAnimationFrame(() => {
                    accordion.style.transition = 'height 0.35s cubic-bezier(0.16, 1, 0.3, 1)';
                    accordion.style.height = `${endHeight}px`;
                });

                setTimeout(() => {
                    accordion.style.height = '';
                    accordion.style.overflow = '';
                    accordion.style.transition = '';
                    isAnimating = false;
                }, 350);
            }
        });
    });

    // License Modal Logic
    const licenseBtns = document.querySelectorAll('[data-license]');
    const licenseModal = document.getElementById('licenseModal');
    const closeLicenseModal = document.getElementById('closeLicenseModal');
    const licenseTextContent = document.getElementById('licenseTextContent');
    const licenseModalTitle = document.getElementById('licenseModalTitle');

    if (licenseBtns.length > 0 && licenseModal) {
        licenseBtns.forEach(btn => {
            btn.addEventListener('click', async () => {
                const licenseFile = btn.getAttribute('data-license');
                const card = btn.closest('.attribution-card');
                const projectName = card ? card.querySelector('.attribution-title').textContent : 'Track N Race';
                
                licenseModalTitle.textContent = `${projectName} License`;
                licenseTextContent.textContent = 'Loading...';
                licenseModal.classList.add('active');
                document.documentElement.classList.add('modal-open');
                document.body.classList.add('modal-open');
                document.documentElement.style.overflow = 'hidden';
                document.body.style.overflow = 'hidden';

                try {
                    const response = await fetch(`assets/licenses/${licenseFile}`);
                    if (!response.ok) throw new Error('Failed to load license');
                    const text = await response.text();
                    licenseTextContent.textContent = text;
                } catch (error) {
                    licenseTextContent.textContent = 'Error loading license text. Please try again later.';
                    console.error(error);
                }
            });
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
    }

    // Fetch latest release version for Download button
    const downloadBtn = document.getElementById('downloadBtn');
    if (downloadBtn) {
        const getCookie = (name) => {
            const value = `; ${document.cookie}`;
            const parts = value.split(`; ${name}=`);
            if (parts.length === 2) return parts.pop().split(';').shift();
            return null;
        };

        const cachedVersion = getCookie('tnr_latest_version');

        const btnSpan = downloadBtn.querySelector('span');

        const updateBtnText = (version) => {
            if (btnSpan) {
                btnSpan.textContent = `Download ${version}`;
            } else {
                downloadBtn.textContent = `Download ${version}`;
            }
        };

        if (cachedVersion) {
            updateBtnText(cachedVersion);
        } else {
            fetch('https://api.github.com/repos/NoGoat/Track-N-Race/releases/latest')
                .then(response => response.json())
                .then(data => {
                    if (data && data.tag_name) {
                        updateBtnText(data.tag_name);
                        document.cookie = `tnr_latest_version=${data.tag_name}; max-age=3600; path=/; SameSite=Lax`;
                    }
                })
                .catch(error => {
                    console.error('Error fetching latest release:', error);
                });
        }
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
