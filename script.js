document.addEventListener('DOMContentLoaded', () => {
    const navButtons = document.querySelectorAll('.nav-btn, .primary-btn');
    
    // Smooth scroll to sections
    navButtons.forEach(btn => {
        btn.addEventListener('click', () => {
            const targetId = btn.getAttribute('data-target');
            const targetSection = document.getElementById(targetId);
            
            if (targetSection) {
                targetSection.scrollIntoView({
                    behavior: 'smooth'
                });
            }
        });
    });

    // Update active nav button on scroll
    const sections = document.querySelectorAll('.section');
    const navItems = document.querySelectorAll('.nav-btn');

    window.addEventListener('scroll', () => {
        let current = '';
        
        sections.forEach(section => {
            const sectionTop = section.offsetTop;
            if (window.scrollY >= (sectionTop - 200)) {
                current = section.getAttribute('id');
            }
        });

        // If scrolled to the bottom of the page, select the last section
        if ((window.innerHeight + Math.round(window.scrollY)) >= document.body.offsetHeight - 20) {
            current = sections[sections.length - 1].getAttribute('id');
        }

        navItems.forEach(item => {
            item.classList.remove('active');
            if (item.getAttribute('data-target') === current) {
                item.classList.add('active');
            }
        });
    });

    // Fade-in elements on scroll
    const observerOptions = {
        root: null,
        rootMargin: '0px',
        threshold: 0.15
    };

    const observer = new IntersectionObserver((entries, observer) => {
        entries.forEach(entry => {
            if (entry.isIntersecting) {
                entry.target.classList.add('visible');
                observer.unobserve(entry.target);
            }
        });
    }, observerOptions);

    // Apply fade-in class to elements and observe them
    const animatedElements = document.querySelectorAll('.hero-content, .hero-visual, .feature-text, .feature-visual');
    animatedElements.forEach(el => {
        el.classList.add('fade-in');
        observer.observe(el);
    });

    // Carousel Logic
    const track = document.querySelector('.carousel-track');
    const slides = Array.from(track?.children || []);
    const nextButton = document.querySelector('.carousel-btn.next');
    const prevButton = document.querySelector('.carousel-btn.prev');
    const dotsNav = document.querySelector('.carousel-indicators');
    const dots = Array.from(dotsNav?.children || []);

    if (track && slides.length > 0) {
        let currentSlideIndex = 0;

        const updateCarousel = (index) => {
            track.style.transform = `translateX(-${index * 100}%)`;
            dots.forEach(dot => dot.classList.remove('active'));
            if (dots[index]) dots[index].classList.add('active');
            currentSlideIndex = index;
        };

        // Initialize height
        setTimeout(() => updateCarousel(0), 100);
        window.addEventListener('resize', () => updateCarousel(currentSlideIndex));

        nextButton?.addEventListener('click', () => {
            let nextIndex = currentSlideIndex + 1;
            if (nextIndex >= slides.length) nextIndex = 0;
            updateCarousel(nextIndex);
        });

        prevButton?.addEventListener('click', () => {
            let prevIndex = currentSlideIndex - 1;
            if (prevIndex < 0) prevIndex = slides.length - 1;
            updateCarousel(prevIndex);
        });

        dotsNav?.addEventListener('click', e => {
            const targetDot = e.target.closest('.dot');
            if (!targetDot) return;
            const index = dots.findIndex(dot => dot === targetDot);
            updateCarousel(index);
        });

        // Auto-play
        let autoPlayInterval = setInterval(() => {
            let nextIndex = currentSlideIndex + 1;
            if (nextIndex >= slides.length) nextIndex = 0;
            updateCarousel(nextIndex);
        }, 5000);

        // Pause auto-play on hover
        const carouselContainer = document.querySelector('.carousel-container');
        carouselContainer?.addEventListener('mouseenter', () => clearInterval(autoPlayInterval));
        carouselContainer?.addEventListener('mouseleave', () => {
            autoPlayInterval = setInterval(() => {
                let nextIndex = currentSlideIndex + 1;
                if (nextIndex >= slides.length) nextIndex = 0;
                updateCarousel(nextIndex);
            }, 5000);
        });
    }

    // Lightbox Logic
    const lightbox = document.getElementById('lightbox');
    const lightboxImg = lightbox?.querySelector('img');
    const lightboxClose = document.getElementById('lightboxClose');
    const clickableImages = document.querySelectorAll('.card-img-wrapper, .mockup-img');

    if (lightbox && lightboxImg) {
        clickableImages.forEach(wrapper => {
            wrapper.addEventListener('click', (e) => {
                const img = wrapper.tagName === 'IMG' ? wrapper : wrapper.querySelector('img');
                if (img) {
                    lightboxImg.src = img.src;
                    lightbox.classList.add('active');
                    document.body.style.overflow = 'hidden'; // Prevent scrolling
                }
            });
        });

        const lightboxScroll = document.getElementById('lightboxScroll');
        const lightboxCenterer = document.getElementById('lightboxCenterer');

        const closeLightbox = () => {
            lightbox.classList.remove('active');
            lightboxImg.classList.remove('zoomed');
            lightboxCenterer.classList.remove('zoomed');
            document.body.style.overflow = '';
        };

        lightboxClose?.addEventListener('click', (e) => {
            e.stopPropagation();
            closeLightbox();
        });

        let isDragging = false;
        let isMoved = false;
        let startX = 0, startY = 0;
        let startScrollLeft = 0, startScrollTop = 0;

        lightboxImg.addEventListener('dragstart', (e) => e.preventDefault());

        lightboxImg.addEventListener('mousedown', (e) => {
            if (!lightboxImg.classList.contains('zoomed')) return;
            e.preventDefault();
            isDragging = true;
            isMoved = false;
            startX = e.clientX;
            startY = e.clientY;
            startScrollLeft = lightboxScroll.scrollLeft;
            startScrollTop = lightboxScroll.scrollTop;
            lightboxImg.style.cursor = 'grabbing';
            lightboxImg.style.transition = 'none';
        });

        window.addEventListener('mousemove', (e) => {
            if (!isDragging) return;
            if (Math.abs(e.clientX - startX) > 3 || Math.abs(e.clientY - startY) > 3) {
                isMoved = true;
            }
            lightboxScroll.scrollLeft = startScrollLeft - (e.clientX - startX);
            lightboxScroll.scrollTop = startScrollTop - (e.clientY - startY);
        });

        window.addEventListener('mouseup', () => {
            if (!isDragging) return;
            isDragging = false;
            lightboxImg.style.cursor = '';
            lightboxImg.style.transition = '';
        });

        lightboxImg.addEventListener('click', (e) => {
            e.stopPropagation();
            if (isMoved) {
                isMoved = false;
                return;
            }
            if (lightboxImg.classList.contains('zoomed')) {
                lightboxImg.classList.remove('zoomed');
                lightboxCenterer.classList.remove('zoomed');
            } else {
                lightboxImg.classList.add('zoomed');
                lightboxCenterer.classList.add('zoomed');
                setTimeout(() => {
                    lightboxScroll.scrollLeft = (lightboxScroll.scrollWidth - lightboxScroll.clientWidth) / 2;
                    lightboxScroll.scrollTop = (lightboxScroll.scrollHeight - lightboxScroll.clientHeight) / 2;
                }, 10);
            }
        });
    }

    // License Modal Logic
    const licenseBtns = document.querySelectorAll('.license-btn');
    const licenseModal = document.getElementById('licenseModal');
    const closeLicenseModal = document.getElementById('closeLicenseModal');
    const licenseTextContent = document.getElementById('licenseTextContent');
    const licenseModalTitle = document.getElementById('licenseModalTitle');

    if (licenseBtns.length > 0 && licenseModal) {
        licenseBtns.forEach(btn => {
            btn.addEventListener('click', async (e) => {
                const licenseFile = btn.getAttribute('data-license');
                const card = btn.closest('.attribution-card');
                const projectName = card ? card.querySelector('.attribution-title').textContent : 'Track-N-Race';
                
                licenseModalTitle.textContent = `${projectName} License`;
                licenseTextContent.textContent = 'Loading...';
                licenseModal.classList.add('active');
                document.body.style.overflow = 'hidden'; // Prevent scrolling

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

        if (cachedVersion) {
            downloadBtn.textContent = `Download ${cachedVersion}`;
        } else {
            fetch('https://api.github.com/repos/NoGoat/Track-N-Race/releases/latest')
                .then(response => response.json())
                .then(data => {
                    if (data && data.tag_name) {
                        downloadBtn.textContent = `Download ${data.tag_name}`;
                        document.cookie = `tnr_latest_version=${data.tag_name}; max-age=3600; path=/; SameSite=Lax`;
                    }
                })
                .catch(error => {
                    console.error('Error fetching latest release:', error);
                });
        }
    }
});
